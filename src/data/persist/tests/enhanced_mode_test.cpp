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
#include <vector>

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
    std::vector<uint8_t> order{9};
    CHECK(save_read_enhanced_mode(&read, version, &player_mask, &order));
    CHECK(version == 1);
    CHECK(player_mask == 0);
    CHECK(order.empty());
    // The application decides which positive rules versions it supports.
    for (const uint32_t recorded : {2u, 3u, 4u, 5u, 6u, 7u, 8u, 10u, 11u}) {
        CHECK(save_write_enhanced_mode(&bank, recorded));
        CHECK(save_read_enhanced_mode(&bank, version));
        CHECK(version == recorded);
        ByteImage saved{};
        CHECK(image_of(bank, saved));
        Bank restored{};
        bank_init(&restored);
        CHECK(bank_read_image(&restored, saved.data, saved.size, nullptr, nullptr, nullptr));
        CHECK(save_read_enhanced_mode(&restored, version));
        CHECK(version == recorded);
        byte_image_free(&saved);
        bank_destroy(&restored);
    }
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
    constexpr std::array<uint8_t, 2> ignored_order{1, 0};
    CHECK(save_write_enhanced_mode(&bank, 0, 3, ignored_order));
    uint32_t version = 99;
    uint16_t player_mask = 77;
    std::vector<uint8_t> order{9};
    CHECK(save_read_enhanced_mode(&bank, version, &player_mask, &order));
    CHECK(version == 0);
    CHECK(player_mask == 0);
    CHECK(order.empty());
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
    std::vector<uint8_t> order;
    CHECK(save_read_enhanced_mode(&read, version, &player_mask, &order));
    CHECK(version == 1);
    CHECK(player_mask == original_players);
    CHECK((order == std::vector<uint8_t>{0, 9}));
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

void physical_player_order_round_trips() {
    Bank bank{};
    start_save(bank);
    constexpr uint16_t original_players =
        (uint16_t{1} << 0) | (uint16_t{1} << 3) | (uint16_t{1} << 9);
    constexpr std::array<uint8_t, 3> physical_order{3, 0, 9};
    CHECK(save_write_enhanced_mode(&bank, 1, original_players, physical_order));
    CHECK(std::strcmp(bank_get_text(&bank, enhanced_mode_key::unit_player_order, ""), "309") == 0);
    ByteImage image{};
    CHECK(image_of(bank, image));
    Bank read{};
    bank_init(&read);
    CHECK(bank_read_image(&read, image.data, image.size, nullptr, nullptr, nullptr));
    uint32_t version{};
    uint16_t player_mask{};
    std::vector<uint8_t> order;
    CHECK(save_read_enhanced_mode(&read, version, &player_mask, &order));
    CHECK(version == 1);
    CHECK(player_mask == original_players);
    CHECK((order == std::vector<uint8_t>{3, 0, 9}));
    CHECK(save_write_enhanced_mode(&read, 1, original_players));
    CHECK(save_read_enhanced_mode(&read, version, &player_mask, &order));
    CHECK((order == std::vector<uint8_t>{0, 3, 9}));
    constexpr std::array<uint8_t, 10> every_player{9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    CHECK(save_write_enhanced_mode(&read, 1, 1023, every_player));
    CHECK(save_read_enhanced_mode(&read, version, &player_mask, &order));
    CHECK(order == std::vector<uint8_t>(every_player.begin(), every_player.end()));
    byte_image_free(&image);
    bank_destroy(&read);
    bank_destroy(&bank);
}

void older_mask_without_order_uses_ascending_slots() {
    Bank bank{};
    start_save(bank);
    CHECK(save_write_enhanced_mode(&bank, 1));
    CHECK(bank_set_int(&bank, enhanced_mode_key::unit_player_mask, 513));
    uint32_t version{};
    uint16_t player_mask{};
    std::vector<uint8_t> order;
    CHECK(save_read_enhanced_mode(&bank, version, &player_mask, &order));
    CHECK(version == 1);
    CHECK(player_mask == 513);
    CHECK((order == std::vector<uint8_t>{0, 9}));
    bank_destroy(&bank);
}

void invalid_player_orders_are_refused() {
    Bank bank{};
    start_save(bank);
    constexpr uint16_t original_players = 11; // Slots 0, 1 and 3.
    CHECK(save_write_enhanced_mode(&bank, 1, original_players));
    uint32_t version = 77;
    uint16_t player_mask = 88;
    std::vector<uint8_t> order{9};
    constexpr std::array<const char*, 8> invalid_text{
        "", "001", "01", "0130", "019", "01A", "01234567890", "3 10"
    };
    for (const char* text : invalid_text) {
        CHECK(bank_set_text(&bank, enhanced_mode_key::unit_player_order, text));
        CHECK(!save_read_enhanced_mode(&bank, version, &player_mask, &order));
        CHECK(version == 77);
        CHECK(player_mask == 88);
        CHECK((order == std::vector<uint8_t>{9}));
        CHECK(!save_read_enhanced_mode(&bank, version));
    }
    CHECK(bank_set_int(&bank, enhanced_mode_key::unit_player_order, 310));
    CHECK(!save_read_enhanced_mode(&bank, version, &player_mask, &order));
    constexpr std::array<uint8_t, 3> duplicate{0, 0, 3};
    constexpr std::array<uint8_t, 3> outside{0, 1, 10};
    constexpr std::array<uint8_t, 3> unallocated{0, 1, 9};
    constexpr std::array<uint8_t, 2> incomplete{0, 1};
    constexpr std::array<uint8_t, 11> too_long{};
    CHECK(!save_write_enhanced_mode(&bank, 1, original_players, duplicate));
    CHECK(!save_write_enhanced_mode(&bank, 1, original_players, outside));
    CHECK(!save_write_enhanced_mode(&bank, 1, original_players, unallocated));
    CHECK(!save_write_enhanced_mode(&bank, 1, original_players, incomplete));
    CHECK(!save_write_enhanced_mode(&bank, 1, original_players, too_long));
    CHECK(!save_write_enhanced_mode(&bank, 1, 0, incomplete));
    bank_destroy(&bank);

    start_save(bank);
    CHECK(save_write_enhanced_mode(&bank, 1));
    CHECK(bank_set_text(&bank, enhanced_mode_key::unit_player_order, "0"));
    CHECK(!save_read_enhanced_mode(&bank, version, &player_mask, &order));
    CHECK(version == 77);
    CHECK(player_mask == 88);
    CHECK((order == std::vector<uint8_t>{9}));
    CHECK(!save_read_enhanced_mode(&bank, version));
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
    physical_player_order_round_trips();
    older_mask_without_order_uses_ascending_slots();
    invalid_player_orders_are_refused();
    invalid_player_masks_are_refused();
    malformed_accounts_are_refused();
    return test::finish("persist-enhanced-mode");
}
