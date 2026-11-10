// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The ModProfile account: what a save written under a mod's profile records
// and reads back through a bank image, and that a save without one reads as
// none.

#include "check.hpp"
#include "oa/base/bytes.hpp"

#include "oa/data/persist/save_profile.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace oa::data::persist;

namespace {

/// Serializes a bank and reads the image into a fresh bank.
///
/// @param bank the bank written
/// @param[out] read the bank read back; reset first
/// @return true when the image wrote and read
bool round_trip(const Bank& bank, Bank& read) {
    ByteImage image{};
    const bool written = bank_write_image(&bank, "Total Annihilation Saved Game", true, &image);
    bool ok = written && bank_read_image(&read, image.data, image.size, nullptr, nullptr, nullptr);
    byte_image_free(&image);
    return ok;
}

void profile_round_trips() {
    Bank bank;
    bank_init(&bank);
    CHECK(bank_reset(&bank));
    bank_open_account(&bank, "Summary");
    bank_set_int(&bank, "Game Time", 30);
    const SavedProfile profile{"a-mod", "1.2", 1, std::string(64, 'a'), std::string(64, 'b')};
    const std::array<uint8_t, 3> reuse{1, 2, 3};
    const std::array<SavedRuleState, 2> tables{{{"reuse", reuse}, {"empty", {}}}};
    CHECK(save_write_profile(&bank, profile, tables));

    Bank read;
    bank_init(&read);
    CHECK(round_trip(bank, read));
    const auto saved = save_read_profile(&read);
    CHECK(saved.has_value());
    if (saved) {
        CHECK(saved->id == "a-mod" && saved->version == "1.2" && saved->catalogue == 1);
        CHECK(saved->sim_hash == profile.sim_hash && saved->full_hash == profile.full_hash);
    }
    const auto bytes = save_read_rule_state(&read, "reuse");
    CHECK(bytes.has_value() && *bytes == std::vector<uint8_t>(reuse.begin(), reuse.end()));
    // A table of no bytes leaves no blob, and reads as none: its state stays as built.
    CHECK(!save_read_rule_state(&read, "empty").has_value());
    CHECK(!save_read_rule_state(&read, "facing").has_value());
    bank_destroy(&read);
    bank_destroy(&bank);
}

/// Keeps the largest distributed route table intact through both bank encodings.
void large_rule_state_tables_round_trip() {
    constexpr std::size_t header_and_admissions = 3200;
    constexpr std::size_t fields = 64;
    constexpr std::size_t metadata_per_field = 80;
    constexpr std::size_t nodes_per_field = 131072;
    constexpr std::size_t bytes_per_node = 5;
    constexpr std::size_t table_bytes =
        header_and_admissions + fields * (metadata_per_field + nodes_per_field * bytes_per_node);
    constexpr std::size_t boundary = 32 * 1024 * 1024;
    static_assert(table_bytes == 41951360 && table_bytes > boundary);
    const SavedProfile profile{"large-state", "7", 1, std::string(64, '7'), std::string(64, 'f')};
    constexpr std::array<uint8_t, 5> neighbour{9, 2, 6, 5, 3};
    for (const bool compressible : {true, false}) {
        std::vector<uint8_t> payload(table_bytes);
        uint32_t noise = 0x6d2b79f5;
        for (std::size_t at = 0; at < payload.size(); ++at) {
            if (compressible) {
                payload[at] = static_cast<uint8_t>((at / 4096) % 251);
            } else {
                noise ^= noise << 13;
                noise ^= noise >> 17;
                noise ^= noise << 5;
                payload[at] = static_cast<uint8_t>(noise >> 24);
            }
        }
        payload.front() = 0xa5;
        payload[boundary - 1] = 0x3c;
        payload[boundary] = 0xc3;
        payload.back() = 0x5a;
        Bank bank{};
        bank_init(&bank);
        CHECK(bank_reset(&bank));
        bank_open_account(&bank, "Summary");
        CHECK(bank_set_int(&bank, "Game Time", 12345));
        const std::array<SavedRuleState, 2> tables{
            {{"shared-routes", payload}, {"neighbour", neighbour}}
        };
        CHECK(save_write_profile(&bank, profile, tables));
        for (const bool pack : {false, true}) {
            ByteImage image{};
            const bool written =
                bank_write_image(&bank, "Total Annihilation Saved Game", pack, &image);
            CHECK(written);
            if (written) {
                // A large incompressible account must survive the packer's
                // bounded failure through the bank's ordinary raw fallback.
                if (pack && compressible)
                    CHECK(image.size < payload.size() / 2);
                else
                    CHECK(image.size > payload.size());
                // Summary is the first record and ModProfile the second.
                // Inspect that account's flag, not just the whole file size.
                CHECK(image.size >= bank_header_bytes + account_header_bytes);
                if (image.size >= bank_header_bytes + account_header_bytes) {
                    const auto summary_size = oa::base::bytes::load_le32(
                        image.data + bank_header_bytes + account_header::size
                    );
                    const auto profile_at = uint64_t{bank_header_bytes} + summary_size;
                    CHECK(profile_at + account_header_bytes <= image.size);
                    if (profile_at + account_header_bytes <= image.size)
                        CHECK(
                            oa::base::bytes::load_le32(
                                image.data + profile_at + account_header::packed
                            ) == (pack && compressible ? 1u : 0u)
                        );
                }
                Bank read{};
                bank_init(&read);
                CHECK(bank_read_image(&read, image.data, image.size, nullptr, nullptr, nullptr));
                const auto saved = save_read_profile(&read);
                CHECK(saved.has_value());
                if (saved) {
                    CHECK(saved->id == profile.id && saved->version == profile.version);
                    CHECK(saved->catalogue == profile.catalogue);
                    CHECK(
                        saved->sim_hash == profile.sim_hash && saved->full_hash == profile.full_hash
                    );
                }
                const auto bytes = save_read_rule_state(&read, "shared-routes");
                CHECK(bytes.has_value() && *bytes == payload);
                const auto beside = save_read_rule_state(&read, "neighbour");
                CHECK(
                    beside.has_value() &&
                    *beside == std::vector<uint8_t>(neighbour.begin(), neighbour.end())
                );
                bank_open_account(&read, "Summary");
                CHECK(bank_get_int(&read, "Game Time", -1) == 12345);
                bank_destroy(&read);
            }
            byte_image_free(&image);
        }
        bank_destroy(&bank);
    }
}

void save_without_profile_has_none() {
    Bank bank;
    bank_init(&bank);
    CHECK(bank_reset(&bank));
    bank_open_account(&bank, "Summary");
    bank_set_int(&bank, "Game Time", 30);
    Bank read;
    bank_init(&read);
    CHECK(round_trip(bank, read));
    CHECK(!save_read_profile(&read).has_value());
    bank_destroy(&read);
    bank_destroy(&bank);
}

} // namespace

int main() {
    profile_round_trips();
    large_rule_state_tables_round_trip();
    save_without_profile_has_none();
    return test::finish("persist-profile");
}
