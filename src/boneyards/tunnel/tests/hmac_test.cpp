// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// HMAC-SHA256. The cases are the Rust crypto tests named below, including the
// RFC 4231 vectors.
#include "oa/boneyards/tunnel/hmac.hpp"
#include "oa/base/sha256.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace {

std::span<const uint8_t> text_bytes(std::string_view text) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool tag_is(const oa::base::sha256::Digest& tag, std::string_view hex) {
    const auto parsed = oa::base::sha256::parse_hex(hex);
    return parsed.has_value() && *parsed == tag;
}

void hmac_sha256_rfc4231_case1() {
    std::array<uint8_t, 20> key{};
    key.fill(0x0b);
    const auto tag = oa::boneyards::tunnel::hmac_sha256(key, text_bytes("Hi There"));
    OA_CHECK(tag_is(tag, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
}

void hmac_sha256_rfc4231_case2() {
    const auto tag = oa::boneyards::tunnel::hmac_sha256(
        text_bytes("Jefe"), text_bytes("what do ya want for nothing?")
    );
    OA_CHECK(tag_is(tag, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
}

void hmac_long_key_is_hashed() {
    std::array<uint8_t, 131> key{};
    key.fill(0xaa);
    const auto tag = oa::boneyards::tunnel::hmac_sha256(
        key, text_bytes("Test Using Larger Than Block-Size Key - Hash Key First")
    );
    OA_CHECK(tag_is(tag, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

void hmac_two_part_equals_concat() {
    const auto key = text_bytes("overlay-key");
    const auto part_one = text_bytes("coreprime-overlay-v1");
    std::array<uint8_t, 16> part_two{};
    part_two.fill(0xa5);
    std::array<uint8_t, 36> joined{};
    for (size_t index = 0; index < part_one.size(); ++index)
        joined[index] = part_one[index];
    for (size_t index = 0; index < part_two.size(); ++index)
        joined[part_one.size() + index] = part_two[index];
    const auto split = oa::boneyards::tunnel::hmac_sha256_two(key, part_one, part_two);
    const auto whole = oa::boneyards::tunnel::hmac_sha256(key, joined);
    OA_CHECK(split == whole);
}

void same_tag_rejects_a_different_byte() {
    const auto tag = oa::boneyards::tunnel::hmac_sha256(text_bytes("Jefe"), text_bytes("Hi"));
    auto other = tag;
    other[0] = static_cast<uint8_t>(other[0] ^ 0x01);
    OA_CHECK(oa::boneyards::tunnel::same_hmac(tag, tag));
    OA_CHECK(!oa::boneyards::tunnel::same_hmac(tag, other));
    OA_CHECK(!oa::boneyards::tunnel::same_hmac(std::span<const uint8_t>(tag.data(), 16), tag));
}

} // namespace

int main() {
    hmac_sha256_rfc4231_case1();
    hmac_sha256_rfc4231_case2();
    hmac_long_key_is_hashed();
    hmac_two_part_equals_concat();
    same_tag_rejects_a_different_byte();
    return oa::test::check_exit_status();
}
