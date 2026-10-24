// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The seat text: a round trip, the exact bytes of two seats, members out of
// order, an unknown member ignored, each broken rule refused, and the level
// a difficulty takes.

#include "oa/data/brain_library/seats.hpp"
#include "oa/test/check.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace lib = oa::data::brain_library;

constexpr std::string_view kSha =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

constexpr std::string_view kTwoSeats =
    R"({"seats":[{"level":"hard","place":0},{"brain":"steady-commander","level":"firm","options":{"aggression":"high"},"package_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","place":1,"version":"1.2"}]})";

/// A seat with the fields a test names.
///
/// @param place the place
/// @param brain the brain; empty is built-in
/// @param level the level; empty is the setup's difficulty
/// @return the seat
lib::Seat seat_of(int32_t place, std::string brain, std::string level) {
    lib::Seat seat;
    seat.place = place;
    seat.brain = std::move(brain);
    seat.level = std::move(level);
    return seat;
}

/// The two-seat record whose text is fixed.
///
/// @return the seats, written out of place order
lib::Seats two_seats() {
    lib::Seats seats;
    lib::Seat second = seat_of(1, "steady-commander", "firm");
    second.version = "1.2";
    second.package_sha256 = std::string{kSha};
    second.options.emplace_back("aggression", "high");
    seats.seats.push_back(std::move(second));
    seats.seats.push_back(seat_of(0, {}, "hard"));
    return seats;
}

void test_round_trip() {
    const lib::Seats seats = two_seats();
    const std::string text = lib::seats_text(seats);
    OA_CHECK(text == kTwoSeats);
    const std::optional<lib::Seats> read = lib::parse_seats(text);
    OA_CHECK(read.has_value());
    if (!read.has_value())
        return;
    OA_CHECK(lib::seats_text(*read) == text);
    // The writer lists seats by place, so the built-in seat is first.
    OA_CHECK(read->seats.size() == 2);
    if (read->seats.size() < 2)
        return;
    OA_CHECK(read->seats[0].place == 0);
    OA_CHECK(read->seats[0].brain.empty());
    OA_CHECK(read->seats[0].level == "hard");
    OA_CHECK(read->seats[0].options.empty());
    OA_CHECK(read->seats[1].place == 1);
    OA_CHECK(read->seats[1].brain == "steady-commander");
    OA_CHECK(read->seats[1].level == "firm");
    OA_CHECK(read->seats[1].version == "1.2");
    OA_CHECK(read->seats[1].package_sha256 == kSha);
    OA_CHECK(read->seats[1].name.empty());
    OA_CHECK(read->seats[1].options.size() == 1);
    if (read->seats[1].options.empty())
        return;
    OA_CHECK(read->seats[1].options[0].first == "aggression");
    OA_CHECK(read->seats[1].options[0].second == "high");

    const std::string example =
        R"({"seats":[{"brain":"steady-commander","level":"firm","options":{"aggression":"high"},"place":1}]})";
    const std::optional<lib::Seats> example_read = lib::parse_seats(example);
    OA_CHECK(example_read.has_value());
    OA_CHECK(lib::seats_text(*example_read) == example);

    lib::Seats ordered;
    lib::Seat zeal = seat_of(3, "steady-commander", "firm");
    zeal.options.emplace_back("zeal", "1");
    zeal.options.emplace_back("aim", "true");
    ordered.seats.push_back(std::move(zeal));
    const std::string kept = lib::seats_text(ordered);
    OA_CHECK(kept.find(R"("options":{"zeal":"1","aim":"true"})") != std::string::npos);
    const std::optional<lib::Seats> kept_read = lib::parse_seats(kept);
    OA_CHECK(kept_read.has_value());
    OA_CHECK(lib::seats_text(*kept_read) == kept);

    lib::Seats named;
    lib::Seat commander = seat_of(1, "steady-commander", "firm");
    commander.name = "Steady Commander";
    commander.version = "1.2";
    named.seats.push_back(std::move(commander));
    const std::string with_name = lib::seats_text(named);
    OA_CHECK(
        with_name ==
        R"({"seats":[{"brain":"steady-commander","level":"firm","name":"Steady Commander","place":1,"version":"1.2"}]})"
    );
    const std::optional<lib::Seats> named_read = lib::parse_seats(with_name);
    OA_CHECK(named_read.has_value());
    OA_CHECK(named_read->seats.size() == 1);
    if (!named_read->seats.empty())
        OA_CHECK(named_read->seats[0].name == "Steady Commander");
}

void test_out_of_order_and_unknown() {
    const std::string shuffled =
        std::string{R"({"note":true,"seats":[{"version":"1.2","place":1,"package_sha256":")"} +
        std::string{kSha} +
        R"(","options":{"aggression":"high"},"level":"firm","brain":"steady-commander","zzz":1},{"extra":"x","place":0,"level":"hard"}]})";
    const std::optional<lib::Seats> read = lib::parse_seats(shuffled);
    OA_CHECK(read.has_value());
    OA_CHECK(lib::seats_text(*read) == kTwoSeats);

    const std::optional<lib::Seats> unknown = lib::parse_seats(
        R"({"note":1,"seats":[{"place":1,"zzz":true,"brain":"steady-commander"}]})"
    );
    OA_CHECK(unknown.has_value());
    OA_CHECK(unknown->seats.size() == 1);
    OA_CHECK(unknown->seats[0].place == 1);
    OA_CHECK(unknown->seats[0].brain == "steady-commander");
    OA_CHECK(unknown->seats[0].level.empty());
    OA_CHECK(unknown->seats[0].options.empty());
    OA_CHECK(lib::seats_text(*unknown) == R"({"seats":[{"brain":"steady-commander","place":1}]})");
}

void refuse(std::string_view text) {
    OA_CHECK(!lib::parse_seats(text).has_value());
}

void test_refusals() {
    refuse("");
    refuse("[]");
    refuse("null");
    refuse("{");
    refuse(R"({"seats":)");
    refuse("{}");
    refuse(R"({"seats":{}})");
    refuse(R"({"seats":[1]})");
    refuse(R"({"seats":[{"brain":"steady-commander"}]})");
    refuse(R"({"seats":[{"place":"1"}]})");
    refuse(R"({"seats":[{"place":1.5}]})");
    refuse(R"({"seats":[{"place":true}]})");
    refuse(R"({"seats":[{"place":-1}]})");
    refuse(R"({"seats":[{"place":10}]})");
    refuse(R"({"seats":[{"place":1},{"place":1}]})");
    refuse(R"({"seats":[{"place":0,"brain":""}]})");
    refuse(R"({"seats":[{"place":0,"brain":"Steady"}]})");
    refuse(R"({"seats":[{"place":0,"brain":"a-"}]})");
    refuse(R"({"seats":[{"place":0,"brain":")" + std::string(65, 'a') + R"("}]})");
    refuse(R"({"seats":[{"place":0,"version":")" + std::string(33, 'a') + R"("}]})");
    refuse(R"({"seats":[{"place":0,"package_sha256":"abc"}]})");
    refuse(R"({"seats":[{"place":0,"package_sha256":")" + std::string(64, 'A') + R"("}]})");
    refuse(R"({"seats":[{"place":0,"package_sha256":")" + std::string(64, 'g') + R"("}]})");
    refuse(R"({"seats":[{"place":0,"level":"Firm"}]})");
    refuse(R"({"seats":[{"place":0,"level":""}]})");
    refuse(R"({"seats":[{"place":0,"name":""}]})");
    refuse("{\"seats\":[{\"place\":0,\"name\":\"bad\x7f\"}]}");
    refuse(R"({"seats":[{"place":0,"name":")" + std::string(65, 'a') + R"("}]})");
    refuse(R"({"seats":[{"place":0,"level":")" + std::string(65, 'a') + R"("}]})");
    refuse(R"({"seats":[{"place":0,"options":[]}]})");
    refuse(R"({"seats":[{"place":0,"options":{"Bad":"high"}}]})");
    refuse(R"({"seats":[{"place":0,"options":{"aggression":1}}]})");
    refuse(R"({"seats":[{"place":0,"options":{"aggression":true}}]})");
    refuse(R"({"seats":[{"place":0,"options":{"aggression":")" + std::string(65, 'a') + R"("}}]})");
    refuse(R"({"seats":[{"place":0,"options":{"aim":"1","aim":"2"}}]})");
    refuse(R"({"seats":[{"place":0,"brain":"steady-commander","brain":"other-brain"}]})");
    refuse(R"({"seats":[],"seats":[{"place":0}]})");

    std::string many = R"({"seats":[)";
    for (int place = 0; place < 11; ++place) {
        if (place != 0)
            many += ',';
        many += R"({"place":)" + std::to_string(place % 10) + '}';
    }
    many += "]}";
    refuse(many);

    std::string options = R"({"seats":[{"place":0,"options":{)";
    for (int option = 0; option < 17; ++option) {
        if (option != 0)
            options += ',';
        options += "\"o" + std::to_string(option) + "\":\"1\"";
    }
    options += "}}]}";
    refuse(options);

    std::string capped = R"({"seats":[]})";
    capped.append(16 * 1024 - capped.size(), ' ');
    OA_CHECK(capped.size() == 16 * 1024);
    const std::optional<lib::Seats> edge = lib::parse_seats(capped);
    OA_CHECK(edge.has_value());
    OA_CHECK(edge->seats.empty());
    capped.push_back(' ');
    refuse(capped);

    const std::optional<lib::Seats> empty_version =
        lib::parse_seats(R"({"seats":[{"place":2,"version":""}]})");
    OA_CHECK(empty_version.has_value());
    OA_CHECK(empty_version->seats.size() == 1);
    OA_CHECK(empty_version->seats[0].version.empty());
    OA_CHECK(lib::seats_text(*empty_version) == R"({"seats":[{"place":2}]})");
}

void test_default_level() {
    constexpr lib::Difficulty steady[] = {
        lib::Difficulty::easy, lib::Difficulty::medium, lib::Difficulty::hard
    };
    OA_CHECK(lib::default_level(steady, lib::Difficulty::easy) == 0);
    OA_CHECK(lib::default_level(steady, lib::Difficulty::medium) == 1);
    OA_CHECK(lib::default_level(steady, lib::Difficulty::hard) == 2);

    constexpr lib::Difficulty ridge[] = {lib::Difficulty::hard};
    OA_CHECK(lib::default_level(ridge, lib::Difficulty::easy) == 0);
    OA_CHECK(lib::default_level(ridge, lib::Difficulty::medium) == 0);
    OA_CHECK(lib::default_level(ridge, lib::Difficulty::hard) == 0);

    OA_CHECK(lib::default_level({}, lib::Difficulty::easy) == 0);
    OA_CHECK(lib::default_level({}, lib::Difficulty::medium) == 0);
    OA_CHECK(lib::default_level({}, lib::Difficulty::hard) == 0);
}

} // namespace

int main() {
    test_round_trip();
    test_out_of_order_and_unknown();
    test_refusals();
    test_default_level();
    return oa::test::check_exit_status();
}
