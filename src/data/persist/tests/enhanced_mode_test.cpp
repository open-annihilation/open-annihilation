// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Enhanced Mode revisions and feature ids survive save round-trips,
// malformed accounts are refused, and a game with the mode off keeps its
// original save bytes.
#include "check.hpp"

#include "oa/data/persist/save_enhanced_mode.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
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

/// One played account: a revision, optional ids, and an optional allocation.
SavedEnhancedMode
played(uint32_t revision, uint16_t mask = 0, std::span<const uint8_t> order = {}) {
    SavedEnhancedMode saved;
    saved.revision = revision;
    saved.unit_player_mask = mask;
    saved.order.assign(order.begin(), order.end());
    return saved;
}

void revision_round_trips() {
    Bank bank{};
    start_save(bank);
    SavedEnhancedMode written = played(1);
    written.feature_ids = {60, 1};
    written.needs_version = "0.9.3";
    CHECK(save_write_enhanced_mode(&bank, written));
    CHECK(std::strcmp(bank_get_text(&bank, enhanced_mode_key::features, ""), "1,60") == 0);
    CHECK(std::strcmp(bank_get_text(&bank, enhanced_mode_key::needs_version, ""), "0.9.3") == 0);
    ByteImage image{};
    CHECK(image_of(bank, image));
    Bank read{};
    bank_init(&read);
    CHECK(bank_read_image(&read, image.data, image.size, nullptr, nullptr, nullptr));
    SavedEnhancedMode saved;
    saved.revision = 99;
    CHECK(save_read_enhanced_mode(&read, saved));
    CHECK(saved.revision == 1);
    CHECK((saved.feature_ids == std::vector<uint32_t>{1, 60}));
    CHECK(saved.needs_version == "0.9.3");
    CHECK(saved.unit_player_mask == 0);
    CHECK(saved.order.empty());
    // The application decides which positive revisions and ids it can play.
    for (const uint32_t recorded : {2u, 3u, 4u}) {
        SavedEnhancedMode again = played(recorded);
        again.feature_ids = {recorded};
        CHECK(save_write_enhanced_mode(&bank, again));
        CHECK(save_read_enhanced_mode(&bank, saved));
        CHECK(saved.revision == recorded);
        CHECK(saved.feature_ids.size() == 1 && saved.feature_ids[0] == recorded);
        CHECK(saved.needs_version.empty());
        CHECK(!bank_has_field(&bank, enhanced_mode_key::needs_version));
        ByteImage stored{};
        CHECK(image_of(bank, stored));
        Bank restored{};
        bank_init(&restored);
        CHECK(bank_read_image(&restored, stored.data, stored.size, nullptr, nullptr, nullptr));
        CHECK(save_read_enhanced_mode(&restored, saved));
        CHECK(saved.revision == recorded);
        byte_image_free(&stored);
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
    CHECK(save_write_enhanced_mode(&bank, SavedEnhancedMode{}));
    SavedEnhancedMode masked = played(0, 3);
    constexpr std::array<uint8_t, 2> ignored_order{1, 0};
    masked.order.assign(ignored_order.begin(), ignored_order.end());
    CHECK(save_write_enhanced_mode(&bank, masked));
    SavedEnhancedMode saved;
    saved.revision = 99;
    saved.unit_player_mask = 77;
    saved.order = {9};
    CHECK(save_read_enhanced_mode(&bank, saved));
    CHECK(saved.revision == 0);
    CHECK(saved.feature_ids.empty());
    CHECK(saved.needs_version.empty());
    CHECK(saved.unit_player_mask == 0);
    CHECK(saved.order.empty());
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

void player_slots_round_trip() {
    Bank bank{};
    start_save(bank);
    // Sparse slots remain allocated even if their players are no longer live.
    constexpr uint16_t kept_slots = (uint16_t{1} << 0) | (uint16_t{1} << 9);
    CHECK(save_write_enhanced_mode(&bank, played(1, kept_slots)));
    ByteImage image{};
    CHECK(image_of(bank, image));
    Bank read{};
    bank_init(&read);
    CHECK(bank_read_image(&read, image.data, image.size, nullptr, nullptr, nullptr));
    SavedEnhancedMode saved;
    CHECK(save_read_enhanced_mode(&read, saved));
    CHECK(saved.revision == 1);
    CHECK(saved.unit_player_mask == kept_slots);
    CHECK((saved.order == std::vector<uint8_t>{0, 9}));
    // A later write that only changes the revision retains the allocation.
    CHECK(save_write_enhanced_mode(&read, played(2)));
    CHECK(save_read_enhanced_mode(&read, saved));
    CHECK(saved.revision == 2);
    CHECK(saved.unit_player_mask == kept_slots);
    constexpr uint16_t all_players = 1023;
    CHECK(save_write_enhanced_mode(&read, played(1, all_players)));
    CHECK(save_read_enhanced_mode(&read, saved));
    CHECK(saved.unit_player_mask == all_players);
    byte_image_free(&image);
    bank_destroy(&read);
    bank_destroy(&bank);
}

void physical_player_order_round_trips() {
    Bank bank{};
    start_save(bank);
    constexpr uint16_t kept_slots = (uint16_t{1} << 0) | (uint16_t{1} << 3) | (uint16_t{1} << 9);
    constexpr std::array<uint8_t, 3> physical_order{3, 0, 9};
    CHECK(save_write_enhanced_mode(&bank, played(1, kept_slots, physical_order)));
    CHECK(std::strcmp(bank_get_text(&bank, enhanced_mode_key::unit_player_order, ""), "309") == 0);
    ByteImage image{};
    CHECK(image_of(bank, image));
    Bank read{};
    bank_init(&read);
    CHECK(bank_read_image(&read, image.data, image.size, nullptr, nullptr, nullptr));
    SavedEnhancedMode saved;
    CHECK(save_read_enhanced_mode(&read, saved));
    CHECK(saved.revision == 1);
    CHECK(saved.unit_player_mask == kept_slots);
    CHECK((saved.order == std::vector<uint8_t>{3, 0, 9}));
    CHECK(save_write_enhanced_mode(&read, played(1, kept_slots)));
    CHECK(save_read_enhanced_mode(&read, saved));
    CHECK((saved.order == std::vector<uint8_t>{0, 3, 9}));
    constexpr std::array<uint8_t, 10> every_player{9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    CHECK(save_write_enhanced_mode(&read, played(1, 1023, every_player)));
    CHECK(save_read_enhanced_mode(&read, saved));
    CHECK(saved.order == std::vector<uint8_t>(every_player.begin(), every_player.end()));
    byte_image_free(&image);
    bank_destroy(&read);
    bank_destroy(&bank);
}

void older_mask_without_order_uses_ascending_slots() {
    Bank bank{};
    start_save(bank);
    CHECK(save_write_enhanced_mode(&bank, played(1)));
    CHECK(bank_set_int(&bank, enhanced_mode_key::unit_player_mask, 513));
    SavedEnhancedMode saved;
    CHECK(save_read_enhanced_mode(&bank, saved));
    CHECK(saved.revision == 1);
    CHECK(saved.unit_player_mask == 513);
    CHECK((saved.order == std::vector<uint8_t>{0, 9}));
    bank_destroy(&bank);
}

void invalid_player_orders_are_refused() {
    Bank bank{};
    start_save(bank);
    constexpr uint16_t kept_slots = 11; // Slots 0, 1 and 3.
    CHECK(save_write_enhanced_mode(&bank, played(1, kept_slots)));
    SavedEnhancedMode saved;
    saved.revision = 77;
    saved.unit_player_mask = 88;
    saved.order = {9};
    constexpr std::array<const char*, 8> invalid_text{
        "", "001", "01", "0130", "019", "01A", "01234567890", "3 10"
    };
    for (const char* text : invalid_text) {
        CHECK(bank_set_text(&bank, enhanced_mode_key::unit_player_order, text));
        CHECK(!save_read_enhanced_mode(&bank, saved));
        CHECK(saved.revision == 77);
        CHECK(saved.unit_player_mask == 88);
        CHECK((saved.order == std::vector<uint8_t>{9}));
    }
    CHECK(bank_set_int(&bank, enhanced_mode_key::unit_player_order, 310));
    CHECK(!save_read_enhanced_mode(&bank, saved));
    constexpr std::array<uint8_t, 3> duplicate{0, 0, 3};
    constexpr std::array<uint8_t, 3> outside{0, 1, 10};
    constexpr std::array<uint8_t, 3> unallocated{0, 1, 9};
    constexpr std::array<uint8_t, 2> incomplete{0, 1};
    constexpr std::array<uint8_t, 11> too_long{};
    CHECK(!save_write_enhanced_mode(&bank, played(1, kept_slots, duplicate)));
    CHECK(!save_write_enhanced_mode(&bank, played(1, kept_slots, outside)));
    CHECK(!save_write_enhanced_mode(&bank, played(1, kept_slots, unallocated)));
    CHECK(!save_write_enhanced_mode(&bank, played(1, kept_slots, incomplete)));
    CHECK(!save_write_enhanced_mode(&bank, played(1, kept_slots, too_long)));
    CHECK(!save_write_enhanced_mode(&bank, played(1, 0, incomplete)));
    bank_destroy(&bank);

    start_save(bank);
    CHECK(save_write_enhanced_mode(&bank, played(1)));
    CHECK(bank_set_text(&bank, enhanced_mode_key::unit_player_order, "0"));
    CHECK(!save_read_enhanced_mode(&bank, saved));
    CHECK(saved.revision == 77);
    CHECK(saved.unit_player_mask == 88);
    CHECK((saved.order == std::vector<uint8_t>{9}));
    bank_destroy(&bank);
}

void invalid_player_masks_are_refused() {
    Bank bank{};
    start_save(bank);
    CHECK(save_write_enhanced_mode(&bank, played(1)));
    constexpr uint16_t slot_past_last_player = uint16_t{1} << 10;
    CHECK(!save_write_enhanced_mode(&bank, played(1, slot_past_last_player)));
    SavedEnhancedMode saved;
    saved.revision = 77;
    saved.unit_player_mask = 88;
    for (const int32_t invalid : std::array<int32_t, 4>{0, -1, 1024, 65535}) {
        CHECK(bank_set_int(&bank, enhanced_mode_key::unit_player_mask, invalid));
        CHECK(!save_read_enhanced_mode(&bank, saved));
        CHECK(saved.revision == 77);
        CHECK(saved.unit_player_mask == 88);
    }
    CHECK(bank_set_text(&bank, enhanced_mode_key::unit_player_mask, "3"));
    CHECK(!save_read_enhanced_mode(&bank, saved));
    CHECK(saved.revision == 77);
    CHECK(saved.unit_player_mask == 88);
    bank_destroy(&bank);
}

void malformed_features_are_refused() {
    Bank bank{};
    start_save(bank);
    CHECK(save_write_enhanced_mode(&bank, played(1)));
    SavedEnhancedMode saved;
    saved.revision = 77;
    constexpr std::array<const char*, 8> invalid_text{
        "0", "01", "1,", ",1", "1,1", "1, 2", "capacity", "1,60,1"
    };
    for (const char* text : invalid_text) {
        CHECK(bank_set_text(&bank, enhanced_mode_key::features, text));
        CHECK(!save_read_enhanced_mode(&bank, saved));
        CHECK(saved.revision == 77);
    }
    CHECK(bank_set_text(&bank, enhanced_mode_key::features, "60,1"));
    CHECK(save_read_enhanced_mode(&bank, saved));
    CHECK((saved.feature_ids == std::vector<uint32_t>{60, 1}));
    bank_destroy(&bank);
}

void malformed_accounts_are_refused() {
    Bank bank{};
    start_save(bank);
    bank_open_account(&bank, enhanced_mode_key::account);
    SavedEnhancedMode saved;
    saved.revision = 77;
    CHECK(bank_set_int(&bank, enhanced_mode_key::format_version, 1));
    CHECK(bank_set_int(&bank, enhanced_mode_key::revision, 1));
    CHECK(!save_read_enhanced_mode(&bank, saved));
    CHECK(saved.revision == 77);
    CHECK(bank_set_int(&bank, enhanced_mode_key::format_version, 2));
    CHECK(bank_set_text(&bank, enhanced_mode_key::revision, "1"));
    CHECK(!save_read_enhanced_mode(&bank, saved));
    CHECK(bank_set_int(&bank, enhanced_mode_key::revision, 0));
    CHECK(!save_read_enhanced_mode(&bank, saved));
    CHECK(bank_set_int(&bank, enhanced_mode_key::revision, -1));
    CHECK(!save_read_enhanced_mode(&bank, saved));
    CHECK(bank_set_text(&bank, enhanced_mode_key::format_version, "2"));
    CHECK(bank_set_int(&bank, enhanced_mode_key::revision, 1));
    CHECK(!save_read_enhanced_mode(&bank, saved));
    CHECK(saved.revision == 77);
    SavedEnhancedMode huge;
    huge.revision = std::numeric_limits<uint32_t>::max();
    CHECK(!save_write_enhanced_mode(&bank, huge));
    SavedEnhancedMode repeated = played(1);
    repeated.feature_ids = {1, 1};
    CHECK(!save_write_enhanced_mode(&bank, repeated));
    CHECK(!save_read_enhanced_mode(nullptr, saved));
    CHECK(!save_write_enhanced_mode(nullptr, SavedEnhancedMode{}));
    bank_destroy(&bank);
}

} // namespace

int main() {
    revision_round_trips();
    mode_off_keeps_save_bytes();
    player_slots_round_trip();
    physical_player_order_round_trips();
    older_mask_without_order_uses_ascending_slots();
    invalid_player_orders_are_refused();
    invalid_player_masks_are_refused();
    malformed_features_are_refused();
    malformed_accounts_are_refused();
    return test::finish("persist-enhanced-mode");
}
