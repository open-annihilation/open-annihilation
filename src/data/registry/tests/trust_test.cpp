// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which registries a player may add. Each numbered rule for a new registry
// has its own case. The built-in registry here is an example, never one that
// ships with the game.

#include "oa/base/signing/ed25519.hpp"
#include "oa/data/catalogue/check.hpp"
#include "oa/data/registry/trust.hpp"
#include "oa/formats/url.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace catalogue = oa::data::catalogue;
namespace registry = oa::data::registry;
namespace signing = oa::base::signing;
namespace url = oa::formats::url;

std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
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

std::string key_text(const signing::PublicKey& key) {
    return std::string(signing::public_key_text(key).view());
}

const signing::PublicKey& builtin_key() {
    static const signing::PublicKey key = key_from_seed(0x11);
    return key;
}

const signing::PublicKey& other_key() {
    static const signing::PublicKey key = key_from_seed(0x22);
    return key;
}

registry::Descriptor read_signed(
    std::string_view id,
    std::string_view name,
    std::string_view catalogue,
    const signing::PublicKey& key,
    std::string_view key_id
) {
    const std::string yaml = "registry: 1\nid: \"" + std::string(id) + "\"\nname: \"" +
                             std::string(name) + "\"\ncatalogue: \"" + std::string(catalogue) +
                             "\"\nkeys:\n  - {id: \"" + std::string(key_id) + "\", public: \"" +
                             key_text(key) + "\"}\ndownloads:\n  mode: direct\n";
    registry::Descriptor descriptor;
    std::string error;
    OA_CHECK(registry::read_descriptor(bytes_of(yaml), descriptor, &error));
    return descriptor;
}

const registry::Descriptor& builtin() {
    static const registry::Descriptor descriptor = [] {
        registry::Descriptor value = read_signed(
            "example",
            "Example",
            "http://downloads.example.net/v1/catalogue.json",
            builtin_key(),
            "example-2026"
        );
        value.homepage = url::parse_web_url("https://home.example.net/");
        if (const auto mirror = url::parse_http_url("http://mirror.example.org/catalogue.json"))
            value.mirrors.push_back(*mirror);
        return value;
    }();
    return descriptor;
}

registry::AddContext base_context(bool developer = false) {
    registry::AddContext context;
    context.built_ins = std::span<const registry::Descriptor>(&builtin(), 1);
    context.developer_mode = developer;
    return context;
}

registry::Descriptor fresh(std::string_view catalogue) {
    return read_signed("other-maps", "Other Maps", catalogue, other_key(), "other-2026");
}

bool same_keys(
    const std::vector<registry::RegistryKey>& left, const std::vector<registry::RegistryKey>& right
) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].id != right[index].id || left[index].key != right[index].key)
            return false;
    }
    return true;
}

registry::Descriptor unsigned_host(std::string host) {
    registry::Descriptor descriptor;
    descriptor.id = "local-maps";
    descriptor.name = "Local Maps";
    descriptor.catalogue.scheme = url::Scheme::http;
    descriptor.catalogue.host = std::move(host);
    descriptor.catalogue.port = 80;
    descriptor.catalogue.target = "/catalogue.json";
    return descriptor;
}

/// Rule 1: a descriptor that cannot be stored is refused, and the list is unchanged.
void rule_1_unreadable_descriptor_is_refused() {
    registry::PlayerRegistries player;
    registry::Descriptor broken = fresh("http://other.example.org/catalogue.json");
    broken.name.clear();
    OA_CHECK(
        registry::add_registry(player, broken, "", "2026-11-03", base_context()) ==
        registry::Refusal::unreadable
    );
    OA_CHECK(player.registries.empty());
    registry::Descriptor sound = fresh("http://other.example.org/catalogue.json");
    OA_CHECK(
        registry::add_registry(player, sound, "", "2026-11-3", base_context()) ==
        registry::Refusal::unreadable
    );
    OA_CHECK(player.registries.empty());
    OA_CHECK(
        registry::add_registry(player, sound, "not-an-address", "2026-11-03", base_context()) ==
        registry::Refusal::unreadable
    );
    OA_CHECK(player.registries.empty());
}

/// Rule 2: an id of a built-in registry is refused in any ASCII case.
void rule_2_builtin_id_is_refused() {
    registry::Descriptor descriptor = fresh("http://other.example.org/catalogue.json");
    descriptor.id = "EXAMPLE";
    OA_CHECK(
        registry::check_new_registry(descriptor, base_context()) == registry::Refusal::built_in_id
    );
}

/// Rule 2: a name of a built-in registry is refused after trimming spaces and ignoring ASCII case.
void rule_2_builtin_name_is_refused() {
    registry::Descriptor descriptor = fresh("http://other.example.org/catalogue.json");
    descriptor.name = "  eXaMpLe  ";
    OA_CHECK(
        registry::check_new_registry(descriptor, base_context()) == registry::Refusal::built_in_name
    );
}

/// Rule 3: a built-in key is refused under any id.
void rule_3_builtin_key_is_refused() {
    registry::Descriptor descriptor = fresh("http://other.example.org/catalogue.json");
    descriptor.keys[0].id = "renamed-2026";
    descriptor.keys[0].key = builtin_key();
    OA_CHECK(
        registry::check_new_registry(descriptor, base_context()) == registry::Refusal::built_in_key
    );
}

/// Rule 4: a mirror whose host lies under a built-in host is refused.
void rule_4_mirror_host_under_builtin_host_is_refused() {
    registry::Descriptor descriptor = fresh("http://other.example.org/catalogue.json");
    if (const auto mirror = url::parse_http_url("http://x.downloads.example.net/catalogue.json"))
        descriptor.mirrors.push_back(*mirror);
    OA_CHECK(
        registry::check_new_registry(descriptor, base_context()) == registry::Refusal::built_in_host
    );
}

/// Rule 4: a homepage on a built-in registry's host is refused.
void rule_4_homepage_host_is_refused() {
    registry::Descriptor descriptor = fresh("http://other.example.org/catalogue.json");
    descriptor.homepage = url::parse_web_url("https://home.example.net/about");
    OA_CHECK(
        registry::check_new_registry(descriptor, base_context()) == registry::Refusal::built_in_host
    );
}

/// Rule 4: an API host under a built-in host is refused.
void rule_4_api_host_is_refused() {
    registry::Descriptor descriptor = fresh("http://other.example.org/catalogue.json");
    descriptor.api = url::parse_http_url("http://x.downloads.example.net/api");
    OA_CHECK(
        registry::check_new_registry(descriptor, base_context()) == registry::Refusal::built_in_host
    );
}

/// Rule 4: the same catalogue host is refused. A parent of that host is not.
void rule_4_host_direction_is_builtin_then_new() {
    registry::Descriptor same = fresh("http://downloads.example.net/other.json");
    OA_CHECK(
        registry::check_new_registry(same, base_context()) == registry::Refusal::built_in_host
    );
    registry::Descriptor parent = fresh("http://example.net/catalogue.json");
    OA_CHECK(registry::check_new_registry(parent, base_context()) == registry::Refusal::none);
}

/// Rule 5: an id already added is refused, before the unsigned rules.
void rule_5_already_added_is_refused() {
    registry::PlayerRegistries player;
    registry::AddedRegistry added;
    added.descriptor.id = "other-maps";
    player.registries.push_back(added);
    registry::AddContext context = base_context();
    context.player = &player;
    registry::Descriptor descriptor = fresh("http://other.example.org/catalogue.json");
    descriptor.id = "OTHER-MAPS";
    OA_CHECK(registry::check_new_registry(descriptor, context) == registry::Refusal::already_added);
    descriptor.keys.clear();
    context.developer_mode = false;
    OA_CHECK(registry::check_new_registry(descriptor, context) == registry::Refusal::already_added);
}

/// Rule 6: an unsigned registry needs Developer mode, even on 127.0.0.1.
void rule_6_unsigned_needs_developer_mode() {
    auto loopback = unsigned_host("127.0.0.1");
    const auto parsed = url::parse_http_url("http://127.0.0.1/catalogue.json");
    OA_CHECK(parsed.has_value());
    if (parsed)
        loopback.catalogue = *parsed;
    OA_CHECK(
        registry::check_new_registry(loopback, base_context(false)) ==
        registry::Refusal::unsigned_needs_developer_mode
    );
    OA_CHECK(
        registry::check_new_registry(unsigned_host("127.1"), base_context(false)) ==
        registry::Refusal::unsigned_needs_developer_mode
    );
}

/// Rule 6: an unsigned registry in Developer mode must use exactly 127.0.0.1.
void rule_6_unsigned_needs_exact_loopback() {
    OA_CHECK(
        registry::check_new_registry(unsigned_host("127.1"), base_context(true)) ==
        registry::Refusal::unsigned_needs_loopback
    );
    OA_CHECK(
        registry::check_new_registry(unsigned_host("localhost"), base_context(true)) ==
        registry::Refusal::unsigned_needs_loopback
    );
    auto loopback = unsigned_host("127.0.0.1");
    const auto parsed = url::parse_http_url("http://127.0.0.1:8080/catalogue.json");
    OA_CHECK(parsed.has_value());
    if (parsed)
        loopback.catalogue = *parsed;
    loopback.homepage = url::parse_web_url("https://maps.example.org/");
    OA_CHECK(registry::loopback_only(loopback));
    OA_CHECK(registry::check_new_registry(loopback, base_context(true)) == registry::Refusal::none);
    if (const auto mirror = url::parse_http_url("http://other.example.org/catalogue.json"))
        loopback.mirrors.push_back(*mirror);
    OA_CHECK(!registry::loopback_only(loopback));
    OA_CHECK(
        registry::check_new_registry(loopback, base_context(true)) ==
        registry::Refusal::unsigned_needs_loopback
    );
}

/// A signed registry on a public host needs no Developer mode.
void signed_registry_is_accepted_without_developer_mode() {
    const registry::Descriptor descriptor = fresh("http://other.example.org/catalogue.json");
    OA_CHECK(
        registry::check_new_registry(descriptor, base_context(false)) == registry::Refusal::none
    );
    registry::PlayerRegistries player;
    OA_CHECK(
        registry::add_registry(
            player,
            descriptor,
            "HTTP://Maps.Example.org/oa/registry.yaml",
            "2026-11-03",
            base_context()
        ) == registry::Refusal::none
    );
    OA_CHECK(player.registries.size() == 1);
    if (!player.registries.empty()) {
        OA_CHECK(player.registries[0].enabled);
        OA_CHECK(player.registries[0].player_mirrors.empty());
        OA_CHECK(player.registries[0].url == "http://maps.example.org/oa/registry.yaml");
        OA_CHECK(player.registries[0].added == "2026-11-03");
    }
}

/// Rule 7: the 64th added registry is the last one accepted.
void rule_7_too_many_registries_is_refused() {
    registry::PlayerRegistries player;
    for (int index = 0; index < 64; ++index) {
        registry::AddedRegistry added;
        added.descriptor.id = "r" + std::to_string(index);
        player.registries.push_back(std::move(added));
    }
    registry::AddContext context = base_context();
    context.player = &player;
    const registry::Descriptor descriptor = fresh("http://other.example.org/catalogue.json");
    OA_CHECK(
        registry::check_new_registry(descriptor, context) == registry::Refusal::too_many_registries
    );
    registry::Descriptor unsigned_one = unsigned_host("127.0.0.1");
    OA_CHECK(
        registry::check_new_registry(unsigned_one, context) ==
        registry::Refusal::unsigned_needs_developer_mode
    );
    player.registries.pop_back();
    OA_CHECK(registry::check_new_registry(descriptor, context) == registry::Refusal::none);
    context.player = nullptr;
    player.registries.resize(64);
    OA_CHECK(registry::check_new_registry(descriptor, context) == registry::Refusal::none);
}

/// A mirror on a built-in registry is refused, ahead of every other mirror rule.
void mirror_on_builtin_is_refused() {
    const auto mirror = url::parse_http_url("http://other.example.org/catalogue.json");
    OA_CHECK(mirror.has_value());
    if (!mirror)
        return;
    registry::PlayerRegistries player;
    registry::AddedRegistry added;
    added.descriptor.id = "example";
    player.registries.push_back(added);
    registry::AddContext context = base_context();
    context.player = &player;
    OA_CHECK(
        registry::check_new_mirror("EXAMPLE", *mirror, context) ==
        registry::Refusal::built_in_mirror
    );
    OA_CHECK(player.registries[0].player_mirrors.empty());
}

/// A mirror must be an http or https address with no query, and not one already listed.
void mirror_address_rules_are_refused() {
    registry::PlayerRegistries player;
    registry::AddedRegistry added;
    added.descriptor = fresh("http://other.example.org/catalogue.json");
    if (const auto listed = url::parse_http_url("http://listed.example.org/catalogue.json"))
        added.descriptor.mirrors.push_back(*listed);
    player.registries.push_back(added);
    registry::AddContext context = base_context();
    context.player = &player;
    if (const auto secure = url::parse_web_url("https://other.example.org/catalogue.json"))
        OA_CHECK(
            registry::check_new_mirror("other-maps", *secure, context) == registry::Refusal::none
        );
    if (const auto query = url::parse_http_url("http://other.example.org/catalogue.json?x=1"))
        OA_CHECK(
            registry::check_new_mirror("other-maps", *query, context) ==
            registry::Refusal::bad_mirror
        );
    if (const auto listed = url::parse_http_url("http://listed.example.org/catalogue.json"))
        OA_CHECK(
            registry::check_new_mirror("other-maps", *listed, context) ==
            registry::Refusal::mirror_listed
        );
    const auto chosen = url::parse_http_url("http://chosen.example.org/catalogue.json");
    OA_CHECK(chosen.has_value());
    if (!chosen)
        return;
    OA_CHECK(registry::check_new_mirror("other-maps", *chosen, context) == registry::Refusal::none);
    OA_CHECK(
        registry::add_player_mirror(player, "other-maps", *chosen, base_context()) ==
        registry::Refusal::none
    );
    OA_CHECK(player.registries[0].player_mirrors.size() == 1);
    OA_CHECK(
        registry::check_new_mirror("other-maps", *chosen, context) ==
        registry::Refusal::mirror_listed
    );
    OA_CHECK(
        registry::add_player_mirror(player, "other-maps", *chosen, base_context()) ==
        registry::Refusal::mirror_listed
    );
    OA_CHECK(player.registries[0].player_mirrors.size() == 1);
    OA_CHECK(
        registry::check_new_mirror("missing", *chosen, context) == registry::Refusal::not_added
    );
    OA_CHECK(
        registry::add_player_mirror(player, "missing", *chosen, base_context()) ==
        registry::Refusal::not_added
    );
    OA_CHECK(player.registries.size() == 1);
}

/// A refused edit leaves the list as it was.
void refused_edits_change_nothing() {
    registry::PlayerRegistries player;
    const registry::Descriptor descriptor = fresh("http://other.example.org/catalogue.json");
    OA_CHECK(
        registry::add_registry(player, descriptor, "", "2026-11-03", base_context()) ==
        registry::Refusal::none
    );
    const auto keys = player.registries[0].descriptor.keys;
    OA_CHECK(registry::remove_registry(player, "missing") == registry::Refusal::not_added);
    OA_CHECK(player.registries.size() == 1);
    OA_CHECK(
        registry::set_enabled(player, base_context().built_ins, "missing", false) ==
        registry::Refusal::not_added
    );
    OA_CHECK(player.registries[0].enabled);
    OA_CHECK(
        registry::replace_trusted_keys(player, "missing", {descriptor.keys[0]}) ==
        registry::Refusal::not_added
    );
    OA_CHECK(
        registry::replace_trusted_keys(player, "other-maps", {}) == registry::Refusal::no_keys
    );
    OA_CHECK(same_keys(player.registries[0].descriptor.keys, keys));
    std::vector<registry::RegistryKey> nine;
    for (uint8_t index = 1; index <= 9; ++index)
        nine.push_back(registry::RegistryKey{"key-" + std::to_string(index), key_from_seed(index)});
    OA_CHECK(
        registry::replace_trusted_keys(player, "OTHER-MAPS", nine) == registry::Refusal::unreadable
    );
    OA_CHECK(same_keys(player.registries[0].descriptor.keys, keys));
    registry::RegistryKey repeated = descriptor.keys[0];
    repeated.id = "other-id";
    OA_CHECK(
        registry::replace_trusted_keys(player, "other-maps", {descriptor.keys[0], repeated}) ==
        registry::Refusal::unreadable
    );
    OA_CHECK(same_keys(player.registries[0].descriptor.keys, keys));
    registry::RegistryKey replacement{"rotated-2026", other_key()};
    OA_CHECK(
        registry::replace_trusted_keys(player, "other-maps", {replacement}) ==
        registry::Refusal::none
    );
    OA_CHECK(player.registries[0].descriptor.keys.size() == 1);
    if (!player.registries[0].descriptor.keys.empty())
        OA_CHECK(player.registries[0].descriptor.keys[0].id == "rotated-2026");
    OA_CHECK(registry::remove_registry(player, "OTHER-MAPS") == registry::Refusal::none);
    OA_CHECK(player.registries.empty());
}

/// Turning a built-in registry off records its own id and leaves an added one alone.
void builtin_enabled_flag_is_the_disabled_list() {
    registry::PlayerRegistries player;
    registry::AddedRegistry added;
    added.descriptor.id = "example";
    added.enabled = true;
    player.registries.push_back(added);
    OA_CHECK(
        registry::set_enabled(player, base_context().built_ins, "EXAMPLE", false) ==
        registry::Refusal::none
    );
    OA_CHECK(player.disabled.size() == 1);
    if (!player.disabled.empty())
        OA_CHECK(player.disabled[0] == "example");
    OA_CHECK(player.registries[0].enabled);
    OA_CHECK(
        registry::set_enabled(player, base_context().built_ins, "example", false) ==
        registry::Refusal::none
    );
    OA_CHECK(player.disabled.size() == 1);
    OA_CHECK(
        registry::set_enabled(player, base_context().built_ins, "Example", true) ==
        registry::Refusal::none
    );
    OA_CHECK(player.disabled.empty());
    OA_CHECK(player.registries[0].enabled);
    OA_CHECK(
        registry::set_enabled(player, base_context().built_ins, "other-maps", false) ==
        registry::Refusal::not_added
    );
}

void write_text(const std::filesystem::path& file, std::string_view text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream output(file, std::ios::binary);
    OA_CHECK(static_cast<bool>(output));
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

struct Scratch {
    std::filesystem::path path{};

    Scratch() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("oa-builtin-" + std::to_string(tick));
        std::filesystem::create_directories(path);
    }

    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

/// Built-in files are read in name order. Unsigned, repeated and non-YAML files are skipped.
void builtin_folder_keeps_signed_files() {

    const Scratch scratch;
    const registry::Descriptor alpha = read_signed(
        "alpha", "Alpha", "http://alpha.example.org/catalogue.json", builtin_key(), "alpha-2026"
    );
    const registry::Descriptor beta = read_signed(
        "beta", "Beta", "http://beta.example.org/catalogue.json", other_key(), "beta-2026"
    );
    registry::Descriptor gamma = read_signed(
        "gamma",
        "Gamma",
        "http://gamma.example.org/catalogue.json",
        key_from_seed(0x33),
        "gamma-2026"
    );
    registry::Descriptor unsigned_gamma = gamma;
    unsigned_gamma.keys.clear();
    write_text(scratch.path / "a-good.yaml", registry::descriptor_text(alpha));
    write_text(scratch.path / "b-unpinned.yaml", registry::descriptor_text(beta));
    write_text(scratch.path / "c-unsigned.yaml", registry::descriptor_text(unsigned_gamma));
    write_text(scratch.path / "d-duplicate.yaml", registry::descriptor_text(alpha));
    write_text(scratch.path / "f-after.yaml", registry::descriptor_text(gamma));
    write_text(scratch.path / "skip.txt", "not a registry\n");
    write_text(scratch.path / "nested" / "z.yaml", registry::descriptor_text(beta));
    std::vector<std::string> problems;
    const auto kept = registry::read_builtin_registries(scratch.path, &problems);
    OA_CHECK(kept.size() == 3);
    if (kept.size() == 3) {
        OA_CHECK(kept[0].id == "alpha");
        OA_CHECK(kept[1].id == "beta");
        OA_CHECK(kept[2].id == "gamma");
    }
    OA_CHECK(problems.size() == 2);
    problems.clear();
    OA_CHECK(registry::read_builtin_registries(scratch.path / "missing", &problems).empty());
    OA_CHECK(problems.size() == 1);
}

/// Pinned keys are honoured only for the ids the table names.
void pinned_keys_are_honoured() {
    const Scratch scratch;
    const registry::Descriptor alpha = read_signed(
        "alpha", "Alpha", "http://alpha.example.org/catalogue.json", builtin_key(), "alpha-2026"
    );
    const registry::Descriptor beta = read_signed(
        "beta", "Beta", "http://beta.example.org/catalogue.json", other_key(), "beta-2026"
    );
    const registry::Descriptor delta = read_signed(
        "delta",
        "Delta",
        "http://delta.example.org/catalogue.json",
        key_from_seed(0x33),
        "delta-2026"
    );
    const auto alpha_text = signing::public_key_text(builtin_key());
    const auto wrong_text = signing::public_key_text(other_key());
    const registry::PinnedKey pins[] = {
        {"alpha", "alpha-2026", alpha_text.view()},
        {"delta", "delta-2026", wrong_text.view()},
        {"epsilon", "epsilon-2026", "not-a-key"},
    };
    const registry::Descriptor epsilon = read_signed(
        "epsilon",
        "Epsilon",
        "http://epsilon.example.org/catalogue.json",
        other_key(),
        "epsilon-2026"
    );
    write_text(scratch.path / "a-good.yaml", registry::descriptor_text(alpha));
    write_text(scratch.path / "b-unpinned.yaml", registry::descriptor_text(beta));
    write_text(scratch.path / "e-mismatch.yaml", registry::descriptor_text(delta));
    write_text(scratch.path / "h-bad.yaml", registry::descriptor_text(epsilon));
    std::vector<std::string> problems;
    const auto kept = registry::read_builtin_registries(scratch.path, &problems, pins);
    OA_CHECK(kept.size() == 2);
    if (kept.size() == 2) {
        OA_CHECK(kept[0].id == "alpha");
        OA_CHECK(kept[1].id == "beta");
    }
    OA_CHECK(problems.size() == 2);
    // The compiled table is Core Prime's two keys. This case passes its own pins.
    const std::span<const registry::PinnedKey> compiled = registry::pinned_keys();
    OA_CHECK(compiled.size() == 2);
    if (compiled.size() == 2) {
        OA_CHECK(compiled[0].registry == "coreprime");
        OA_CHECK(compiled[0].key_id == "steve-2026");
        OA_CHECK(compiled[0].public_key == "ed25519:dHXqUJM9LbifF+boPakeKsZoSD29qhTEAxkyRj+s5yU=");
        OA_CHECK(compiled[1].registry == "coreprime");
        OA_CHECK(compiled[1].key_id == "steve-2026-secondary");
        OA_CHECK(compiled[1].public_key == "ed25519:SjxpBROn5onP1ILIa4tQQ3aL5BnghJ7L60Y97nA6YJY=");
    }
}

/// A test key standing in for either pinned id signs a built-in catalogue.
/// A third id is refused.
void either_pinned_id_signs_a_builtin_catalogue() {
    const std::span<const registry::PinnedKey> compiled = registry::pinned_keys();
    OA_CHECK(compiled.size() == 2);
    if (compiled.size() != 2)
        return;
    const std::optional<signing::PublicKey> first =
        signing::parse_public_key("ed25519:25lf4lFp0UHKubu6krqgH58uHs599MsqwFGQ83/MH50=");
    const std::optional<signing::PublicKey> second =
        signing::parse_public_key("ed25519:IVL40Zt5HSRFMkLhXy6rbLfP+ntqXtMAl5YOBpiB2xI=");
    OA_CHECK(first.has_value() && second.has_value());
    if (!first || !second)
        return;
    const registry::RegistryKey trusted[] = {
        {std::string(compiled[0].key_id), *first},
        {std::string(compiled[1].key_id), *second},
    };
    const std::vector<uint8_t> catalogue = bytes_of(
        "{\"catalogue\":1,\"registry\":\"coreprime\",\"sequence\":1,"
        "\"generated\":\"2026-01-01T00:00:00Z\",\"expires\":\"2026-02-01T00:00:00Z\","
        "\"packages\":[]}"
    );
    const std::vector<uint8_t> first_signature = bytes_of(
        "ed25519 steve-2026 "
        "yabxP0VDF9dFEJ4vmhzDvlC7DuhtlzpXyNQYETh3y03MaI0j1myqBugHkWC5nCQ/Abs623khia/S8ibgKcDoAQ==\n"
    );
    const std::vector<uint8_t> second_signature = bytes_of(
        "ed25519 steve-2026-secondary "
        "N/Ju06fQMrjGW1pvc7Fz73laf0tU66LynBKzBWHBcAfWTxX+zsoJdkRas4ezIiumJeaYtkoknRKw8Tkdy1mYBw==\n"
    );
    const std::vector<uint8_t> third_signature = bytes_of(
        "ed25519 third-2026 "
        "z1Tx7XPWA2/UDh0lcMRWQL2HYO6gaeXfExl53ndq9srmYlbGwYthmC2tESuc0GsvvNwe/t4Xb6lk9CTgd+kRCg==\n"
    );
    catalogue::CheckRequest request;
    request.catalogue = catalogue;
    request.registry_id = "coreprime";
    request.trusted_keys = trusted;
    request.built_in = true;
    request.signature = first_signature;
    const catalogue::Checked by_first = catalogue::check_catalogue(request);
    OA_CHECK(by_first.verdict == catalogue::Verdict::accepted);
    OA_CHECK(by_first.signed_by == compiled[0].key_id);
    OA_CHECK(!by_first.new_keys);
    request.signature = second_signature;
    const catalogue::Checked by_second = catalogue::check_catalogue(request);
    OA_CHECK(by_second.verdict == catalogue::Verdict::accepted);
    OA_CHECK(by_second.signed_by == compiled[1].key_id);
    OA_CHECK(!by_second.new_keys);
    request.signature = third_signature;
    const catalogue::Checked by_third = catalogue::check_catalogue(request);
    OA_CHECK(by_third.verdict == catalogue::Verdict::unknown_key);
}

/// Built-in registries come first. A disabled one is off. An added id a built-in has taken conflicts.
void registries_in_effect_order_disabled_and_conflicting() {
    const registry::Descriptor second = read_signed(
        "second", "Second", "http://second.example.org/catalogue.json", other_key(), "second-2026"
    );
    const registry::Descriptor builtins[] = {builtin(), second};
    registry::PlayerRegistries player;
    player.disabled.push_back("EXAMPLE");
    player.disabled.push_back("other-maps");
    registry::AddedRegistry conflicting;
    conflicting.descriptor = fresh("http://other.example.org/catalogue.json");
    conflicting.descriptor.id = "example";
    conflicting.enabled = true;
    conflicting.url = "http://maps.example.org/oa/registry.yaml";
    if (const auto mirror = url::parse_http_url("http://chosen.example.org/catalogue.json"))
        conflicting.player_mirrors.push_back(*mirror);
    registry::AddedRegistry added = conflicting;
    added.descriptor.id = "other-maps";
    added.descriptor.name = "Other Maps";
    added.enabled = false;
    player.registries.push_back(conflicting);
    player.registries.push_back(added);
    const auto effect = registry::registries_in_effect(builtins, player);
    OA_CHECK(effect.size() == 4);
    if (effect.size() != 4)
        return;
    OA_CHECK(effect[0].origin == registry::Origin::built_in);
    OA_CHECK(effect[0].descriptor.id == "example");
    OA_CHECK(!effect[0].enabled);
    OA_CHECK(!effect[0].conflicting);
    OA_CHECK(effect[0].url.empty());
    OA_CHECK(effect[0].player_mirrors.empty());
    OA_CHECK(effect[1].descriptor.id == "second");
    OA_CHECK(effect[1].enabled);
    OA_CHECK(effect[2].origin == registry::Origin::added);
    OA_CHECK(effect[2].conflicting);
    OA_CHECK(effect[2].enabled);
    OA_CHECK(effect[2].url == conflicting.url);
    OA_CHECK(effect[2].player_mirrors.size() == 1);
    OA_CHECK(effect[3].descriptor.id == "other-maps");
    OA_CHECK(!effect[3].conflicting);
    OA_CHECK(!effect[3].enabled);
}

/// Fingerprints are the signing fingerprint, in key order.
void fingerprints_match_the_signing_fingerprint() {
    registry::Descriptor descriptor = fresh("http://other.example.org/catalogue.json");
    descriptor.keys.push_back(registry::RegistryKey{"second-2026", builtin_key()});
    const auto fingerprints = registry::key_fingerprints(descriptor);
    OA_CHECK(fingerprints.size() == 2);
    if (fingerprints.size() == 2) {
        OA_CHECK(fingerprints[0] == signing::fingerprint(other_key()).view());
        OA_CHECK(fingerprints[1] == signing::fingerprint(builtin_key()).view());
    }
}

void added_registry_cannot_claim_the_shipped_logo() {
    registry::Descriptor descriptor = fresh("http://other.example.org/catalogue.json");
    OA_CHECK(registry::check_new_registry(descriptor, base_context()) == registry::Refusal::none);
    const std::string yaml = "registry: 1\nid: other-maps\nname: Other Maps\nicon: oa\n"
                             "catalogue: \"http://other.example.org/catalogue.json\"\n"
                             "keys:\n  - {id: other-2026, public: \"" +
                             key_text(other_key()) + "\"}\ndownloads:\n  mode: direct\n";
    registry::Descriptor claimed;
    std::string error;
    OA_CHECK(registry::read_descriptor(bytes_of(yaml), claimed, &error));
    OA_CHECK(claimed.shipped_logo);
    OA_CHECK(
        registry::check_new_registry(claimed, base_context()) == registry::Refusal::shipped_logo
    );
    const char* sentence = registry::refusal_text(registry::Refusal::shipped_logo);
    OA_CHECK(sentence != nullptr && std::string(sentence).find("logo") != std::string::npos);
}

void every_refusal_has_a_sentence() {
    const registry::Refusal refusals[] = {
        registry::Refusal::none,
        registry::Refusal::unreadable,
        registry::Refusal::built_in_id,
        registry::Refusal::built_in_name,
        registry::Refusal::built_in_key,
        registry::Refusal::built_in_host,
        registry::Refusal::already_added,
        registry::Refusal::unsigned_needs_developer_mode,
        registry::Refusal::unsigned_needs_loopback,
        registry::Refusal::too_many_registries,
        registry::Refusal::not_added,
        registry::Refusal::built_in_mirror,
        registry::Refusal::bad_mirror,
        registry::Refusal::mirror_listed,
        registry::Refusal::no_keys,
        registry::Refusal::shipped_logo,
    };
    for (const registry::Refusal refusal : refusals) {
        const char* text = registry::refusal_text(refusal);
        OA_CHECK(text != nullptr);
        if (text != nullptr)
            OA_CHECK(text[0] != '\0');
    }
}

} // namespace

int main() {
    rule_1_unreadable_descriptor_is_refused();
    rule_2_builtin_id_is_refused();
    rule_2_builtin_name_is_refused();
    rule_3_builtin_key_is_refused();
    rule_4_mirror_host_under_builtin_host_is_refused();
    rule_4_homepage_host_is_refused();
    rule_4_api_host_is_refused();
    rule_4_host_direction_is_builtin_then_new();
    rule_5_already_added_is_refused();
    rule_6_unsigned_needs_developer_mode();
    rule_6_unsigned_needs_exact_loopback();
    signed_registry_is_accepted_without_developer_mode();
    rule_7_too_many_registries_is_refused();
    mirror_on_builtin_is_refused();
    mirror_address_rules_are_refused();
    refused_edits_change_nothing();
    builtin_enabled_flag_is_the_disabled_list();
    builtin_folder_keeps_signed_files();
    pinned_keys_are_honoured();
    either_pinned_id_signs_a_builtin_catalogue();
    registries_in_effect_order_disabled_and_conflicting();
    fingerprints_match_the_signing_fingerprint();
    added_registry_cannot_claim_the_shipped_logo();
    every_refusal_has_a_sentence();
    return oa::test::check_exit_status();
}
