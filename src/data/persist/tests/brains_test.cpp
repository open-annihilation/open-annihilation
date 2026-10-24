// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Brains account: seats and states written and read back through a bank
// image, a state over the limit skipped, each refusal leaving the bank as it
// was, and a save written by the profile steps staying byte for byte the
// same when the account is not asked for.

#include "check.hpp"

#include "oa/data/persist/save_brains.hpp"
#include "oa/data/persist/save_profile.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <tuple>
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

/// The steps persist-profile uses to write a save under a profile.
///
/// @param[out] bank the bank; initialised here
void profile_steps(Bank& bank) {
    bank_init(&bank);
    CHECK(bank_reset(&bank));
    bank_open_account(&bank, "Summary");
    bank_set_int(&bank, "Game Time", 30);
    const SavedProfile profile{"a-mod", "1.2", 1, std::string(64, 'a'), std::string(64, 'b')};
    const std::array<uint8_t, 3> reuse{1, 2, 3};
    const std::array<SavedRuleState, 2> tables{{{"reuse", reuse}, {"empty", {}}}};
    CHECK(save_write_profile(&bank, profile, tables));
}

/// One state of the given player and size.
///
/// @param player the player
/// @param size how many bytes
/// @param mark the byte every entry holds
/// @return the state
SavedBrainState state_of(uint8_t player, std::size_t size, uint8_t mark) {
    SavedBrainState state;
    state.player = player;
    state.bytes.assign(size, mark);
    return state;
}

/// Writes the bank's image.
///
/// @param bank the bank
/// @param[out] image the image; freed by the caller
/// @return true when the image wrote
bool take_image(const Bank& bank, ByteImage& image) {
    return bank_write_image(&bank, "Total Annihilation Saved Game", true, &image);
}

void two_states_round_trip() {
    const SavedBrainState later = state_of(3, 4, 9);
    const SavedBrainState earlier = state_of(1, 2, 7);
    const SavedBrainState empty = state_of(0, 0, 0);
    const SavedBrainState states[] = {later, empty, earlier};
    Bank bank;
    bank_init(&bank);
    CHECK(bank_reset(&bank));
    bank_open_account(&bank, "Summary");
    bank_set_int(&bank, "Game Time", 30);
    CHECK(save_write_brains(&bank, "seats-text", states));
    Bank read;
    bank_init(&read);
    CHECK(round_trip(bank, read));
    const auto loaded = save_read_brains(&read);
    CHECK(loaded.has_value());
    if (loaded.has_value()) {
        CHECK(loaded->seats_text == "seats-text");
        CHECK(loaded->states.size() == 2);
        if (loaded->states.size() == 2) {
            CHECK(loaded->states[0].player == 1);
            CHECK(loaded->states[0].bytes == earlier.bytes);
            CHECK(loaded->states[1].player == 3);
            CHECK(loaded->states[1].bytes == later.bytes);
        }
    }
    bank_destroy(&read);
    bank_destroy(&bank);
}

void one_mebibyte_state_round_trips() {
    const SavedBrainState full = state_of(2, save_brain_state_most_bytes, 4);
    Bank bank;
    bank_init(&bank);
    CHECK(bank_reset(&bank));
    bank_open_account(&bank, "Summary");
    CHECK(save_write_brains(&bank, "seats", std::span<const SavedBrainState>(&full, 1)));
    Bank read;
    bank_init(&read);
    CHECK(round_trip(bank, read));
    const auto loaded = save_read_brains(&read);
    CHECK(loaded.has_value());
    if (loaded.has_value()) {
        CHECK(loaded->states.size() == 1);
        if (!loaded->states.empty()) {
            CHECK(loaded->states[0].player == 2);
            CHECK(loaded->states[0].bytes.size() == save_brain_state_most_bytes);
            CHECK(loaded->states[0].bytes == full.bytes);
        }
    }
    bank_destroy(&read);
    bank_destroy(&bank);
}

void refusals_leave_the_bank() {
    Bank bank;
    profile_steps(bank);
    ByteImage before{};
    CHECK(take_image(bank, before));
    const SavedBrainState one = state_of(1, 3, 1);
    const SavedBrainState again = state_of(1, 1, 2);
    const SavedBrainState high = state_of(10, 1, 3);
    const SavedBrainState huge = state_of(4, save_brain_state_most_bytes + 1, 5);
    const SavedBrainState pair[] = {one, again};
    CHECK(!save_write_brains(&bank, {}, {}));
    CHECK(!save_write_brains(&bank, std::string(save_brain_seats_most_bytes + 1, 'q'), {}));
    CHECK(!save_write_brains(&bank, "seats", std::span<const SavedBrainState>(&high, 1)));
    CHECK(!save_write_brains(&bank, "seats", pair));
    CHECK(!save_write_brains(&bank, "seats", std::span<const SavedBrainState>(&huge, 1)));
    CHECK(!save_write_brains(nullptr, "seats", {}));
    ByteImage after{};
    CHECK(take_image(bank, after));
    CHECK(
        before.size > 0 && before.size == after.size && before.data != nullptr &&
        after.data != nullptr && std::memcmp(before.data, after.data, before.size) == 0
    );
    byte_image_free(&before);
    byte_image_free(&after);
    bank_destroy(&bank);
}

void save_without_brains_reads_none() {
    Bank bank;
    bank_init(&bank);
    CHECK(bank_reset(&bank));
    bank_open_account(&bank, "Summary");
    bank_set_int(&bank, "Game Time", 30);
    Bank read;
    bank_init(&read);
    CHECK(round_trip(bank, read));
    CHECK(!save_read_brains(&read).has_value());
    CHECK(!save_read_brains(nullptr).has_value());
    bank_destroy(&read);
    bank_destroy(&bank);
}

void over_long_state_is_skipped() {
    Bank bank;
    bank_init(&bank);
    CHECK(bank_reset(&bank));
    const SavedBrainState kept = state_of(1, 2, 8);
    CHECK(save_write_brains(&bank, "seats-text", std::span<const SavedBrainState>(&kept, 1)));
    std::ignore = bank_open_account(&bank, brains_key::account.data());
    CHECK(bank_find_blob_name(&bank, "State9", true) >= 0);
    bank_open_blob_name(&bank, "State9");
    const std::string huge(save_brain_state_most_bytes + 1, 'q');
    const auto bytes = static_cast<uint32_t>(huge.size());
    CHECK(bank_blob_write(&bank, huge.data(), bytes) == bytes);
    CHECK(bank_find_blob_name(&bank, "Notes", true) >= 0);
    bank_open_blob_name(&bank, "Notes");
    CHECK(bank_blob_write(&bank, "other", 5) == 5);
    const auto loaded = save_read_brains(&bank);
    CHECK(loaded.has_value());
    if (loaded.has_value()) {
        CHECK(loaded->seats_text == "seats-text");
        CHECK(loaded->states.size() == 1);
        if (!loaded->states.empty()) {
            CHECK(loaded->states[0].player == 1);
            CHECK(loaded->states[0].bytes == kept.bytes);
        }
    }
    bank_destroy(&bank);
}

void over_long_seats_read_none() {
    Bank bank;
    bank_init(&bank);
    CHECK(bank_reset(&bank));
    std::ignore = bank_open_account(&bank, brains_key::account.data());
    CHECK(bank_find_blob_name(&bank, brains_key::seats.data(), true) >= 0);
    bank_open_blob_name(&bank, brains_key::seats.data());
    const std::string huge(save_brain_seats_most_bytes + 1, 'q');
    const auto bytes = static_cast<uint32_t>(huge.size());
    CHECK(bank_blob_write(&bank, huge.data(), bytes) == bytes);
    CHECK(!save_read_brains(&bank).has_value());
    bank_destroy(&bank);
}

void bytes_match_profile_steps_without_the_account() {
    Bank profile_bank;
    profile_steps(profile_bank);
    Bank same;
    profile_steps(same);
    ByteImage left{};
    ByteImage right{};
    CHECK(take_image(profile_bank, left));
    CHECK(take_image(same, right));
    CHECK(
        left.size > 0 && left.size == right.size && left.data != nullptr && right.data != nullptr &&
        std::memcmp(left.data, right.data, left.size) == 0
    );
    const SavedBrainState state = state_of(1, 1, 1);
    CHECK(save_write_brains(&same, "seats", std::span<const SavedBrainState>(&state, 1)));
    ByteImage with_brains{};
    CHECK(take_image(same, with_brains));
    const bool same_bytes = with_brains.size == left.size && with_brains.data != nullptr &&
                            left.data != nullptr &&
                            std::memcmp(with_brains.data, left.data, left.size) == 0;
    CHECK(!same_bytes);
    byte_image_free(&left);
    byte_image_free(&right);
    byte_image_free(&with_brains);
    bank_destroy(&profile_bank);
    bank_destroy(&same);
}

} // namespace

int main() {
    two_states_round_trip();
    one_mebibyte_state_round_trips();
    refusals_leave_the_bank();
    save_without_brains_reads_none();
    over_long_state_is_skipped();
    over_long_seats_read_none();
    bytes_match_profile_steps_without_the_account();
    return test::finish("persist-brains");
}
