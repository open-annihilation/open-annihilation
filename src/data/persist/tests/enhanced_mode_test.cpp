// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Enhanced Mode rules versions survive save round-trips, malformed accounts
// are refused, and a game with the mode off keeps its original save bytes.
#include "check.hpp"

#include "oa/data/persist/save_enhanced_mode.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

using namespace oa::data::persist;

namespace {

/// Starts a save with one ordinary Summary field.
void start_save(Bank& bank) {
    bank_init(&bank);
    CHECK(bank_reset(&bank));
    bank_open_account(&bank, "Summary");
    CHECK(bank_set_int(&bank, "Game Time", 30));
}

/// Writes a bank to its saved-game image.
bool image_of(const Bank& bank, ByteImage& image) {
    return bank_write_image(&bank, "Total Annihilation Saved Game", true, &image);
}

void rules_version_round_trips() {
    Bank bank{};
    start_save(bank);
    CHECK(save_write_enhanced_mode(&bank, 1));
    ByteImage image{};
    CHECK(image_of(bank, image));
    Bank read{};
    bank_init(&read);
    CHECK(bank_read_image(&read, image.data, image.size, nullptr, nullptr, nullptr));
    uint32_t version = 99;
    uint16_t player_mask = 77;
    CHECK(save_read_enhanced_mode(&read, version, &player_mask));
    CHECK(version == 1);
    CHECK(player_mask == 0);
    CHECK(save_write_enhanced_mode(&bank, 2));
    CHECK(save_read_enhanced_mode(&bank, version));
    CHECK(version == 2); // The application decides which rules versions it supports.
    byte_image_free(&image);
    bank_destroy(&read);
    bank_destroy(&bank);
}

void mode_off_keeps_save_bytes() {
    Bank bank{};
    start_save(bank);
    ByteImage before{};
    CHECK(image_of(bank, before));
    CHECK(save_write_enhanced_mode(&bank, 0));
    CHECK(save_write_enhanced_mode(&bank, 0, 3));
    uint32_t version = 99;
    uint16_t player_mask = 77;
    CHECK(save_read_enhanced_mode(&bank, version, &player_mask));
    CHECK(version == 0);
    CHECK(player_mask == 0);
    CHECK(save_read_enhanced_mode(&bank, version));
    CHECK(version == 0);
    ByteImage after{};
    CHECK(image_of(bank, after));
    CHECK(before.size == after.size);
    CHECK(before.data != nullptr && after.data != nullptr);
    if (before.size == after.size && before.data != nullptr && after.data != nullptr)
        CHECK(std::memcmp(before.data, after.data, before.size) == 0);
    byte_image_free(&before);
    byte_image_free(&after);
    bank_destroy(&bank);
}

void original_player_slots_round_trip() {
    Bank bank{};
    start_save(bank);
    // Sparse slots remain allocated even if their players are no longer live.
    constexpr uint16_t original_players = (uint16_t{1} << 0) | (uint16_t{1} << 9);
    CHECK(save_write_enhanced_mode(&bank, 1, original_players));
    ByteImage image{};
    CHECK(image_of(bank, image));
    Bank read{};
    bank_init(&read);
    CHECK(bank_read_image(&read, image.data, image.size, nullptr, nullptr, nullptr));
    uint32_t version{};
    uint16_t player_mask{};
    CHECK(save_read_enhanced_mode(&read, version, &player_mask));
    CHECK(version == 1);
    CHECK(player_mask == original_players);
    // An older caller that only changes the rules version retains the allocation.
    CHECK(save_write_enhanced_mode(&read, 2));
    CHECK(save_read_enhanced_mode(&read, version, &player_mask));
    CHECK(version == 2);
    CHECK(player_mask == original_players);
    constexpr uint16_t all_players = 1023;
    CHECK(save_write_enhanced_mode(&read, 1, all_players));
    CHECK(save_read_enhanced_mode(&read, version, &player_mask));
    CHECK(player_mask == all_players);
    byte_image_free(&image);
    bank_destroy(&read);
    bank_destroy(&bank);
}

void invalid_player_masks_are_refused() {
    Bank bank{};
    start_save(bank);
    CHECK(save_write_enhanced_mode(&bank, 1));
    constexpr uint16_t slot_past_last_player = uint16_t{1} << 10;
    CHECK(!save_write_enhanced_mode(&bank, 1, slot_past_last_player));
    uint32_t version = 77;
    uint16_t player_mask = 88;
    for (const int32_t invalid : std::array<int32_t, 4>{0, -1, 1024, 65535}) {
        CHECK(bank_set_int(&bank, enhanced_mode_key::unit_player_mask, invalid));
        CHECK(!save_read_enhanced_mode(&bank, version, &player_mask));
        CHECK(version == 77);
        CHECK(player_mask == 88);
        CHECK(!save_read_enhanced_mode(&bank, version));
    }
    CHECK(bank_set_text(&bank, enhanced_mode_key::unit_player_mask, "3"));
    CHECK(!save_read_enhanced_mode(&bank, version, &player_mask));
    CHECK(version == 77);
    CHECK(player_mask == 88);
    bank_destroy(&bank);
}

void malformed_accounts_are_refused() {
    Bank bank{};
    start_save(bank);
    bank_open_account(&bank, enhanced_mode_key::account);
    uint32_t version = 77;
    CHECK(bank_set_int(&bank, enhanced_mode_key::format_version, 1));
    CHECK(!save_read_enhanced_mode(&bank, version));
    CHECK(version == 77);
    CHECK(bank_set_text(&bank, enhanced_mode_key::rules_version, "1"));
    CHECK(!save_read_enhanced_mode(&bank, version));
    CHECK(bank_set_int(&bank, enhanced_mode_key::rules_version, 0));
    CHECK(!save_read_enhanced_mode(&bank, version));
    CHECK(bank_set_int(&bank, enhanced_mode_key::rules_version, -1));
    CHECK(!save_read_enhanced_mode(&bank, version));
    CHECK(bank_set_int(&bank, enhanced_mode_key::rules_version, 1));
    CHECK(bank_set_int(&bank, enhanced_mode_key::format_version, 2));
    CHECK(!save_read_enhanced_mode(&bank, version));
    CHECK(bank_set_text(&bank, enhanced_mode_key::format_version, "1"));
    CHECK(!save_read_enhanced_mode(&bank, version));
    CHECK(version == 77);
    CHECK(!save_write_enhanced_mode(&bank, std::numeric_limits<uint32_t>::max()));
    CHECK(!save_read_enhanced_mode(nullptr, version));
    CHECK(!save_write_enhanced_mode(nullptr, 0));
    bank_destroy(&bank);
}

} // namespace

int main() {
    rules_version_round_trips();
    mode_off_keeps_save_bytes();
    original_player_slots_round_trip();
    invalid_player_masks_are_refused();
    malformed_accounts_are_refused();
    return test::finish("persist-enhanced-mode");
}
