// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The seats a brain match accepts, the tick it ends on, and the lines it prints.

#include "oa/app/brain_match.hpp"

#include "oa/test/check.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <sstream>
#include <string>
#include <string_view>

namespace {

namespace match = oa::app;

void expect_seats(
    std::string_view seats,
    std::string_view levels,
    std::string_view first,
    std::string_view second,
    std::string_view level_first,
    std::string_view level_second
) {
    std::string error;
    const auto parsed = match::parse_brain_match(seats, levels, error);
    OA_CHECK(error.empty());
    OA_CHECK(parsed.has_value());
    if (!parsed)
        return;
    OA_CHECK((*parsed)[0].spec == first);
    OA_CHECK((*parsed)[1].spec == second);
    OA_CHECK((*parsed)[0].level == level_first);
    OA_CHECK((*parsed)[1].level == level_second);
}

void parsing_accepts_ids_folders_and_levels() {
    expect_seats("builtin,builtin", "", "builtin", "builtin", "", "");
    expect_seats("steady-commander,idle-brain", "", "steady-commander", "idle-brain", "", "");
    expect_seats(
        "/tmp/steady-commander,builtin",
        "firm,hard",
        "/tmp/steady-commander",
        "builtin",
        "firm",
        "hard"
    );
    expect_seats(
        "brains\\steady-commander,./idle-brain",
        "",
        "brains\\steady-commander",
        "./idle-brain",
        "",
        ""
    );
    expect_seats(
        "steady-commander,builtin", "easy,hard", "steady-commander", "builtin", "easy", "hard"
    );
    expect_seats(
        "steady-commander,idle-brain", "nope,calm", "steady-commander", "idle-brain", "nope", "calm"
    );

    OA_CHECK(match::brain_match_builtin("builtin"));
    OA_CHECK(!match::brain_match_builtin("builtin "));
    OA_CHECK(match::brain_match_folder("/tmp/steady-commander"));
    OA_CHECK(match::brain_match_folder("brains\\steady-commander"));
    OA_CHECK(match::brain_match_folder("./idle-brain"));
    OA_CHECK(!match::brain_match_folder("steady-commander"));
    OA_CHECK(!match::brain_match_folder("builtin"));
}

void parsing_refuses_a_bad_shape() {
    std::string error;
    OA_CHECK(!match::parse_brain_match("builtin", "", error));
    OA_CHECK(error == "--brain-match expects two seats");
    OA_CHECK(!match::parse_brain_match("a,b,c", "", error));
    OA_CHECK(error == "--brain-match expects two seats");
    OA_CHECK(!match::parse_brain_match("builtin,", "", error));
    OA_CHECK(error == "--brain-match has an empty seat");
    OA_CHECK(!match::parse_brain_match(",builtin", "", error));
    OA_CHECK(error == "--brain-match has an empty seat");
    OA_CHECK(!match::parse_brain_match("Not An Id,builtin", "", error));
    OA_CHECK(error == "--brain-match: Not An Id is not a brain id");
    OA_CHECK(!match::parse_brain_match(" builtin,builtin", "", error));
    OA_CHECK(error == "--brain-match:  builtin is not a brain id");
    OA_CHECK(!match::parse_brain_match("builtin,builtin", "easy", error));
    OA_CHECK(error == "--brain-levels expects two levels");
    OA_CHECK(!match::parse_brain_match("builtin,builtin", "easy,hard,firm", error));
    OA_CHECK(error == "--brain-levels expects two levels");
    OA_CHECK(!match::parse_brain_match("builtin,builtin", "easy,", error));
    OA_CHECK(error == "--brain-levels has an empty level");
    OA_CHECK(!match::parse_brain_match("builtin,builtin", "silly,hard", error));
    OA_CHECK(error == "--brain-levels: a built-in level is easy, medium or hard");
    OA_CHECK(!match::parse_brain_match("builtin,builtin", "easy,Hard", error));
    OA_CHECK(error == "--brain-levels: a built-in level is easy, medium or hard");
}

void the_end_rule_names_the_winner() {
    uint8_t winner = 7;
    const std::array<uint32_t, 2> both{3, 3};
    OA_CHECK(match::brain_match_end(both, 1, 10, winner) == match::BrainMatchEnd::going);
    OA_CHECK(winner == 7);

    const std::array<uint32_t, 2> left{2, 0};
    OA_CHECK(match::brain_match_end(left, 10, 10, winner) == match::BrainMatchEnd::last_standing);
    OA_CHECK(winner == 0);
    const std::array<uint32_t, 2> right{0, 4};
    OA_CHECK(match::brain_match_end(right, 3, 10, winner) == match::BrainMatchEnd::last_standing);
    OA_CHECK(winner == 1);

    const std::array<uint32_t, 2> none{0, 0};
    OA_CHECK(match::brain_match_end(none, 10, 10, winner) == match::BrainMatchEnd::draw);
    OA_CHECK(winner == 255);
    winner = 4;
    OA_CHECK(match::brain_match_end(none, 2, 10, winner) == match::BrainMatchEnd::draw);
    OA_CHECK(winner == 255);

    winner = 4;
    OA_CHECK(match::brain_match_end(both, 10, 10, winner) == match::BrainMatchEnd::time);
    OA_CHECK(winner == 255);
    winner = 4;
    OA_CHECK(match::brain_match_end(both, 11, 10, winner) == match::BrainMatchEnd::time);
    OA_CHECK(winner == 255);
    winner = 4;
    OA_CHECK(match::brain_match_end(both, 9, 10, winner) == match::BrainMatchEnd::going);
    OA_CHECK(winner == 4);
}

void whole_numbers_truncate_toward_zero() {
    OA_CHECK(match::brain_match_whole(512.9) == 512);
    OA_CHECK(match::brain_match_whole(-1.9) == -1);
    OA_CHECK(match::brain_match_whole(0.9) == 0);
    OA_CHECK(match::brain_match_whole(-0.2) == 0);
}

void report_lines_match_byte_for_byte() {
    std::ostringstream out;
    match::write_brain_match_heading(out, "Ashap Plateau", 4242, 1800);
    match::write_brain_match_brain(out, 1, "steady-commander", "1.2", "steady", 0);
    match::write_brain_match_builtin(out, 2, "hard", 1);
    match::write_brain_match_end(out, 1800, 255, match::BrainMatchEnd::time);
    match::write_brain_match_totals(
        out, 1, 4, 3, 0, 0, 512, 980, 1203, 4410, true, 1834000, "playing"
    );
    match::write_brain_match_totals(
        out, 2, 9, 8, 0, 0, 220, 1500, 1650, 5120, false, 0, "built-in"
    );
    match::write_brain_match_digest(out, 0x0123456789abcdefULL);
    const std::string sample =
        "brain match: map Ashap Plateau seed 4242 ticks 1800\n"
        "brain match: seat 1 steady-commander 1.2 level steady side 0\n"
        "brain match: seat 2 built-in level hard side 1\n"
        "brain match: end tick 1800 winner none reason time\n"
        "brain match: seat 1 units 4 built 3 lost 0 kills 0 metal 512 energy 980 "
        "metal-produced 1203 energy-produced 4410 instructions 1834000 status playing\n"
        "brain match: seat 2 units 9 built 8 lost 0 kills 0 metal 220 energy 1500 "
        "metal-produced 1650 energy-produced 5120 instructions - status built-in\n"
        "brain match: digest 0123456789abcdef\n";
    OA_CHECK(out.str() == sample);

    std::ostringstream more;
    match::write_brain_match_end(more, 400, 0, match::BrainMatchEnd::last_standing);
    match::write_brain_match_end(more, 400, 1, match::BrainMatchEnd::last_standing);
    match::write_brain_match_end(more, 12, 255, match::BrainMatchEnd::draw);
    match::write_brain_match_stopped(more, 1, "steady-commander: tick: brain.lua:4: breaks");
    match::write_brain_match_unseated(more, 2, "ridge-raider", "the package folder is missing");
    match::write_brain_match_unplayable(more, "Not A Map");
    match::write_brain_match_totals(more, 1, 1, 1, 0, 0, 0, 0, 0, 0, true, 1000, "stopped-at 300");
    const std::string extra =
        "brain match: end tick 400 winner seat 1 reason last-standing\n"
        "brain match: end tick 400 winner seat 2 reason last-standing\n"
        "brain match: end tick 12 winner none reason draw\n"
        "brain match: seat 1 stopped: steady-commander: tick: brain.lua:4: breaks\n"
        "brain match: seat 2 ridge-raider was not seated: the package folder is missing\n"
        "brain match: map Not A Map could not be played\n"
        "brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 0 energy 0 "
        "metal-produced 0 energy-produced 0 instructions 1000 status stopped-at 300\n";
    OA_CHECK(more.str() == extra);
}

} // namespace

int main() {
    try {
        parsing_accepts_ids_folders_and_levels();
        parsing_refuses_a_bad_shape();
        the_end_rule_names_the_winner();
        whole_numbers_truncate_toward_zero();
        report_lines_match_byte_for_byte();
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "app-brain-match: %s\n", failure.what());
        return 1;
    }
    return oa::test::check_exit_status();
}
