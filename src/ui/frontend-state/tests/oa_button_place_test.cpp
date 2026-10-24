// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Places Mutators against the skirmish setup, the campaign launch and the
// battle room, and MORE MAPS…'s block, against those screens' own gadgets.
// The rectangles are the screens' own gadgets at 640 by 480. At one and a
// half times the points per frame pixel, scaling the rectangles and the
// block's sizes keeps the button under Difficulty and above "Set Difficulty".
// The live 1280 by 720 window scales the rectangles only, and NEWGAME's
// block then sits under Difficulty and above Previous Menu.

#include "oa/ui/frontend_state/oa_button_place.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace place = oa::ui::frontend_state;

namespace {

void check(bool held, const char* message) {
    if (!held)
        throw std::runtime_error(message);
}

place::PlaceRect rect(int32_t x, int32_t y, int32_t width, int32_t height) {
    return {x, y, width, height};
}

place::PlaceRect scale_rect(place::PlaceRect source) {
    return {source.x * 3 / 2, source.y * 3 / 2, source.width * 3 / 2, source.height * 3 / 2};
}

place::BlockSizes sizes_of(std::size_t lines) {
    return {80, 17, 12, 200, 80, 4, lines};
}

place::BlockSizes scale_sizes(place::BlockSizes sizes) {
    sizes.button_width = sizes.button_width * 3 / 2;
    sizes.button_height = sizes.button_height * 3 / 2;
    sizes.line_height = sizes.line_height * 3 / 2;
    sizes.line_width = sizes.line_width * 3 / 2;
    sizes.least_line_width = sizes.least_line_width * 3 / 2;
    sizes.gap = sizes.gap * 3 / 2;
    sizes.beside = sizes.beside * 3 / 2;
    return sizes;
}

bool meet(const place::PlaceRect& first, const place::PlaceRect& second) {
    return first.x < second.x + second.width && second.x < first.x + first.width &&
           first.y < second.y + second.height && second.y < first.y + first.height;
}

bool inside(const place::PlaceRect& box, const place::PlaceRect& room) {
    return box.x >= room.x && box.y >= room.y && box.x + box.width <= room.x + room.width &&
           box.y + box.height <= room.y + room.height;
}

void expect_clear(
    const place::BlockPlace& placed,
    const std::vector<place::PlaceRect>& others,
    const place::PlaceRect& room,
    int32_t right
) {
    check(inside(placed.button, room), "the button leaves the frame");
    check(
        placed.button.x + placed.button.width == right, "the button's right edge left the anchor"
    );
    for (const place::PlaceRect& other : others)
        check(!meet(placed.button, other), "the button meets a gadget");
    for (const place::PlaceRect& line : placed.lines) {
        check(inside(line, room), "a line leaves the frame");
        check(line.x + line.width == right, "a line's right edge left the anchor");
        check(!meet(line, placed.button), "a line meets the button");
        for (const place::PlaceRect& other : others)
            check(!meet(line, other), "a line meets a gadget");
    }
}

std::vector<place::PlaceRect> skirmish_others() {
    return {
        rect(476, 76, 120, 12),
        rect(476, 96, 120, 20),
        rect(476, 132, 120, 12),
        rect(476, 153, 120, 20),
        rect(476, 189, 120, 12),
        rect(476, 210, 120, 20),
        rect(476, 246, 120, 12),
        rect(476, 267, 120, 20),
        rect(476, 303, 120, 12),
        rect(476, 379, 120, 20),
        rect(502, 430, 96, 31),
        rect(263, 379, 120, 20),
        rect(47, 382, 195, 15),
        rect(42, 330, 447, 20),
    };
}

std::vector<place::PlaceRect> newgame_others() {
    return {
        rect(463, 308, 117, 12),
        rect(461, 372, 119, 19),
        rect(486, 416, 95, 29),
        rect(34, 314, 318, 142),
        rect(369, 314, 15, 136),
        rect(34, 386, 318, 78),
        rect(369, 388, 15, 78),
        rect(177, 74, 132, 106),
        rect(424, 74, 129, 104),
        rect(209, 250, 95, 20),
        rect(456, 250, 95, 20),
    };
}

void expect_skirmish(
    const place::BlockSizes& sizes, const place::PlaceRect& anchor, const place::PlaceRect& room
) {
    const std::vector<place::PlaceRect> others = [&] {
        std::vector<place::PlaceRect> listed = skirmish_others();
        if (sizes.gap != 4) {
            for (place::PlaceRect& gadget : listed)
                gadget = scale_rect(gadget);
        }
        return listed;
    }();
    const place::BlockPlace placed = place::place_block(anchor, others, room, sizes);
    expect_clear(placed, others, room, anchor.x + anchor.width);
    check(placed.lines.size() == 1, "skirmish keeps its line");
    check(
        placed.button.y == anchor.y + anchor.height + sizes.gap, "skirmish sits under Difficulty"
    );
    const place::PlaceRect& line = placed.lines.front();
    check(line.y == placed.button.y + placed.button.height + 1, "the line sits under the button");
    check(
        line.y + line.height < 379 * (sizes.gap == 4 ? 1 : 3) / (sizes.gap == 4 ? 1 : 2),
        "the line meets Previous"
    );
}

void expect_newgame(
    const place::BlockSizes& sizes, const place::PlaceRect& anchor, const place::PlaceRect& room
) {
    std::vector<place::PlaceRect> others = newgame_others();
    place::PlaceRect label = others.front();
    if (sizes.gap != 4) {
        for (place::PlaceRect& gadget : others)
            gadget = scale_rect(gadget);
        label = scale_rect(label);
    }
    const place::BlockPlace placed = place::place_block(anchor, others, room, sizes);
    expect_clear(placed, others, room, anchor.x + anchor.width);
    check(placed.lines.size() == 1, "the campaign keeps its line");
    check(placed.button.y < anchor.y, "the campaign button stays above Difficulty");
    const place::PlaceRect& line = placed.lines.front();
    check(line.y + line.height <= label.y, "the block stays above Set Difficulty");
}

/// The live 1280 by 720 window: frame rectangles at 3/2, block sizes as at 640.
void expect_newgame_window() {
    const place::BlockSizes sizes = sizes_of(1);
    const place::PlaceRect room = rect(0, 0, 960, 720);
    const place::PlaceRect anchor = scale_rect(rect(461, 327, 108, 20));
    std::vector<place::PlaceRect> others = newgame_others();
    for (place::PlaceRect& gadget : others)
        gadget = scale_rect(gadget);
    const place::PlaceRect previous = others[1];
    const place::BlockPlace placed = place::place_block(anchor, others, room, sizes);
    expect_clear(placed, others, room, anchor.x + anchor.width);
    check(placed.lines.size() == 1, "the 1280 campaign keeps its line");
    check(
        placed.button.y == anchor.y + anchor.height + sizes.gap,
        "the 1280 campaign sits under Difficulty"
    );
    const place::PlaceRect& line = placed.lines.front();
    check(line.y + line.height <= previous.y, "the 1280 campaign stays above Previous Menu");
}

void screen_tests() {
    const place::PlaceRect room = rect(0, 0, 640, 480);
    const place::BlockSizes sizes = sizes_of(1);
    const place::PlaceRect skirmish = rect(476, 324, 120, 20);
    const place::BlockPlace under = place::place_block(skirmish, skirmish_others(), room, sizes);
    check(under.button.y == 348, "skirmish button top");
    check(under.button.x == 516, "skirmish button left");
    check(under.button.height == 17, "skirmish button height");
    check(under.lines.size() == 1, "skirmish line");
    check(under.lines.front().y == 366, "skirmish line top");
    check(under.lines.front().y + under.lines.front().height == 378, "skirmish line bottom");
    expect_skirmish(sizes, skirmish, room);

    const place::PlaceRect campaign = rect(461, 327, 108, 20);
    const place::BlockPlace over = place::place_block(campaign, newgame_others(), room, sizes);
    check(over.button.y == 274, "campaign button top");
    check(over.button.x + over.button.width == 569, "campaign button right");
    check(over.lines.size() == 1, "campaign line");
    check(over.lines.front().y + over.lines.front().height == 304, "campaign block bottom");
    expect_newgame(sizes, campaign, room);

    const place::PlaceRect wide = rect(0, 0, 960, 720);
    expect_skirmish(scale_sizes(sizes), scale_rect(skirmish), wide);
    expect_newgame(scale_sizes(sizes), scale_rect(campaign), wide);
    expect_newgame_window();
}

void shorten_test() {
    const place::PlaceRect anchor = rect(200, 40, 80, 20);
    const place::PlaceRect room = rect(0, 0, 400, 400);
    const place::BlockSizes sizes = sizes_of(1);
    // The line would run from x 100 to 280. A rectangle covers its left side.
    const std::vector<place::PlaceRect> others{rect(80, 78, 50, 12)};
    const place::BlockPlace placed = place::place_block(anchor, others, room, sizes);
    check(placed.lines.size() == 1, "a shortened line is kept");
    const place::PlaceRect& line = placed.lines.front();
    check(line.x + line.width == 280, "a shortened line keeps the right edge");
    check(line.x == 130, "a shortened line ends before the rectangle");
    check(line.width >= sizes.least_line_width, "a shortened line keeps its least width");
    check(!meet(line, others.front()), "a shortened line still meets the rectangle");
}

void drop_test() {
    const place::PlaceRect room = rect(0, 100, 400, 50);
    const place::PlaceRect anchor = rect(200, 100, 80, 20);
    const place::BlockSizes sizes = sizes_of(1);
    const place::BlockPlace placed = place::place_block(anchor, {}, room, sizes);
    check(placed.lines.empty(), "the line that leaves the room is dropped");
    check(placed.button.y == 124, "the button stays under the anchor");
    check(placed.button.y + placed.button.height <= 150, "the button stays in the room");
}

void no_room_test() {
    const place::PlaceRect room = rect(0, 0, 10, 10);
    const place::PlaceRect anchor = rect(100, 100, 40, 20);
    const place::BlockSizes sizes = sizes_of(1);
    const place::BlockPlace placed = place::place_block(anchor, {}, room, sizes);
    check(placed.button.y == 124, "no room still uses the place under the anchor");
    check(placed.lines.size() == 1, "no room still offers the line");
}

void beside_test() {
    const place::PlaceRect anchor = rect(200, 40, 80, 20);
    const place::PlaceRect room = rect(0, 0, 400, 400);
    place::BlockSizes sizes = sizes_of(1);
    sizes.beside = 20;
    // The place under the anchor puts the chip strip at x 280, y 64.
    const std::vector<place::PlaceRect> others{rect(284, 64, 12, 16)};
    const place::BlockPlace placed = place::place_block(anchor, others, room, sizes);
    const place::PlaceRect side{
        placed.button.x + placed.button.width, placed.button.y, sizes.beside, placed.button.height
    };
    check(!meet(side, others.front()), "a gadget on the chip left the strip on it");
    check(placed.button.y < 64, "a gadget on the chip did not move the block");
    check(inside(side, room), "the moved chip strip leaves the room");
    check(
        placed.button.x + placed.button.width == anchor.x + anchor.width,
        "the moved button left the anchor's right edge"
    );
}

/// LOUNGE2's controls, apart from Select Map, in frame pixels.
std::vector<place::PlaceRect> lounge_obstacles() {
    std::vector<place::PlaceRect> obstacles;
    for (int32_t row = 0; row < 10; ++row)
        obstacles.push_back(rect(9, 69 + row * 20, 486, 20));
    const place::PlaceRect column[] = {
        rect(510, 12, 120, 20),
        rect(510, 94, 120, 20),
        rect(510, 134, 120, 20),
        rect(510, 175, 120, 20),
        rect(510, 215, 120, 20),
        rect(510, 255, 120, 20),
        rect(510, 295, 120, 20),
        rect(510, 335, 120, 20),
        rect(510, 374, 120, 20),
        rect(510, 396, 120, 20),
        rect(510, 416, 120, 20),
    };
    for (const place::PlaceRect& one : column)
        obstacles.push_back(one);
    obstacles.push_back(rect(110, 308, 191, 13));
    obstacles.push_back(rect(21, 342, 414, 96));
    obstacles.push_back(rect(21, 441, 294, 20));
    obstacles.push_back(rect(522, 437, 96, 31));
    obstacles.push_back(rect(508, 437, 125, 30));
    obstacles.push_back(rect(9, 456, 447, 20));
    return obstacles;
}

/// One listed width places the same button a single button_width does.
void one_width_matches() {
    const place::PlaceRect room = rect(0, 0, 640, 480);
    const place::PlaceRect skirmish = rect(476, 324, 120, 20);
    const place::BlockSizes plain = sizes_of(1);
    place::BlockSizes listed = plain;
    listed.button_widths = {plain.button_width};
    const place::BlockPlace single = place::place_block(skirmish, skirmish_others(), room, plain);
    const place::BlockPlace named = place::place_block(skirmish, skirmish_others(), room, listed);
    check(named.buttons.size() == 1, "one listed width is one button");
    check(
        named.button.x == single.button.x && named.button.y == single.button.y &&
            named.button.width == single.button.width &&
            named.button.height == single.button.height,
        "one listed width matches the single button"
    );
    check(named.lines.size() == single.lines.size(), "one listed width keeps the same lines");
    check(
        !named.lines.empty() && named.lines.front().x == single.lines.front().x &&
            named.lines.front().y == single.lines.front().y &&
            named.lines.front().width == single.lines.front().width,
        "one listed width keeps the line"
    );
}

/// Two buttons on LOUNGE2, with GET MAP's box among the others and without it.
void two_button_row() {
    const place::PlaceRect room = rect(0, 0, 640, 480);
    const place::PlaceRect map = rect(327, 306, 120, 20);
    place::BlockSizes sizes = sizes_of(1);
    sizes.button_width = 52;
    sizes.button_widths = {64, 96};
    sizes.button_gap = 5;
    const std::vector<place::PlaceRect> obstacles = lounge_obstacles();
    const place::BlockPlace clear = place::place_block(map, obstacles, room, sizes);
    check(clear.buttons.size() == 2, "two widths are two buttons");
    check(
        clear.buttons.front().x + 64 + 5 == clear.buttons.back().x, "the buttons stand a gap apart"
    );
    check(
        clear.button.x == clear.buttons.back().x && clear.button.y == clear.buttons.back().y &&
            clear.button.width == clear.buttons.back().width &&
            clear.button.height == clear.buttons.back().height,
        "the rightmost button is the row"
    );
    expect_clear(clear, obstacles, room, map.x + map.width);
    check(clear.button.y == 272, "without GET MAP the row sits under the player rows");

    std::vector<place::PlaceRect> crowded = obstacles;
    crowded.push_back(rect(205, 283, 96, 17));
    crowded.push_back(rect(110, 327, 191, 12));
    const place::BlockPlace beside = place::place_block(map, crowded, room, sizes);
    check(beside.buttons.size() == 2, "GET MAP's box keeps two buttons");
    expect_clear(beside, crowded, room, map.x + map.width);
    check(beside.button.y == 35, "GET MAP's box lifts the row clear");
    check(beside.lines.size() == 1, "GET MAP's box keeps the line");
}

void column_test() {
    const place::PlaceRect room = rect(0, 0, 640, 480);
    const place::PlaceRect difficulty = rect(476, 324, 120, 20);
    const std::optional<place::PlaceRect> previous =
        place::next_below(difficulty, skirmish_others());
    check(previous.has_value() && previous->y == 379, "Previous Menu is under Difficulty");
    const std::optional<place::PlaceRect> placed =
        place::place_in_column(difficulty, skirmish_others(), room, 2);
    check(placed.has_value(), "the column holds the button");
    check(placed->x == 476, "the button's left edge is the column's");
    check(placed->width == 120 && placed->height == 20, "the button is as large as Previous Menu");
    const int32_t above = placed->y - 344;
    const int32_t below = 379 - (placed->y + placed->height);
    check(
        above == 7 && (below == above || below == above + 1),
        "the button is centred between Difficulty and Previous Menu"
    );

    // A gadget in the space leaves no place.
    std::vector<place::PlaceRect> blocked = skirmish_others();
    blocked.push_back(rect(500, 352, 40, 12));
    check(
        !place::place_in_column(difficulty, blocked, room, 2), "a gadget in the space is refused"
    );
    // A space too short for the button with its gaps leaves no place.
    check(
        !place::place_in_column(rect(476, 344, 120, 20), skirmish_others(), room, 2),
        "a short space is refused"
    );
}

void middle_test() {
    const place::PlaceRect room = rect(0, 0, 640, 480);
    const place::PlaceRect arm = rect(209, 250, 95, 20);
    const place::PlaceRect core = rect(456, 250, 95, 20);
    const place::PlaceRect middle = place::bar_middle(arm, core);
    check(middle.x == 313 && middle.width == 134, "the middle is set in by the bolts");
    check(middle.y == 250 && middle.height == 20, "the middle is as tall as the bar");
    const std::optional<place::PlaceRect> placed =
        place::place_under_middle(arm, core, newgame_others(), room);
    check(placed.has_value(), "the bar holds the button under its middle");
    check(placed->x == 313 && placed->width == 134 && placed->height == 20, "the middle's size");
    check(placed->y == 279, "the button is a bolt under the bar");
    check(placed->y + placed->height <= 308, "the button stays above Set Difficulty");

    std::vector<place::PlaceRect> blocked = newgame_others();
    blocked.push_back(rect(300, 282, 160, 10));
    check(
        !place::place_under_middle(arm, core, blocked, room), "a gadget under the bar is refused"
    );
}

void over_test() {
    const place::PlaceRect room = rect(0, 0, 640, 480);
    const place::PlaceRect map = rect(327, 306, 120, 20);
    const std::vector<place::PlaceRect> others{
        rect(9, 249, 486, 20),   // the last player row
        rect(110, 308, 191, 13), // the map's name
    };
    const std::optional<place::PlaceRect> placed = place::place_over(map, others, room, 2);
    check(placed.has_value(), "the room holds the button over Select Map");
    check(placed->x == 327 && placed->width == 120 && placed->height == 20, "Select Map's size");
    check(placed->y == 277, "the button is centred between the rows and Select Map");

    const std::vector<place::PlaceRect> crowded{rect(9, 249, 486, 45)};
    check(!place::place_over(map, crowded, room, 2), "a short space over Select Map is refused");
}

void focus_test() {
    const std::vector<place::PlaceRect> records{
        rect(476, 324, 120, 20),
        rect(263, 379, 120, 20),
        rect(476, 379, 120, 20),
    };
    const place::PlaceRect button = rect(516, 348, 80, 17);
    check(place::focus_place(records, button) == 1, "the button follows Difficulty");
    const place::PlaceRect tied = rect(516, 348, 10, 10);
    check(
        place::focus_place(std::span<const place::PlaceRect>(&tied, 1), button) == 1,
        "an equal key comes first"
    );
}

/// One skirmish row's gadgets, plus the options column, at 640 by 480.
///
/// The rows are the setup's own: Player, Side, Color, Allies, Metal and
/// Energy, spread the way the setup spreads them. The options column is
/// Commander Death, Start Location, Mapping, Line of Sight, Difficulty,
/// their labels, and Previous Menu.
std::vector<place::PlaceRect> skirmish_rows(int32_t slots) {
    std::vector<place::PlaceRect> gadgets{
        rect(476, 76, 120, 12),
        rect(476, 96, 120, 20),
        rect(476, 132, 120, 12),
        rect(476, 153, 120, 20),
        rect(476, 189, 120, 12),
        rect(476, 210, 120, 20),
        rect(476, 246, 120, 12),
        rect(476, 267, 120, 20),
        rect(476, 303, 120, 12),
        rect(476, 324, 120, 20),
        rect(476, 379, 120, 20),
    };
    const int32_t spacing = 200 / slots;
    const int32_t first = (180 - (slots - 1) * spacing) / 2 + 79;
    for (int32_t row = 0; row < slots; ++row) {
        const int32_t y = first + row * spacing;
        gadgets.push_back(rect(45, y, 112, 20));
        gadgets.push_back(rect(163, y, 45, 20));
        gadgets.push_back(rect(214, y, 20, 20));
        gadgets.push_back(rect(241, y, 40, 20));
        gadgets.push_back(rect(286, y, 45, 20));
        gadgets.push_back(rect(337, y, 45, 20));
    }
    return gadgets;
}

void expect_beside(
    const place::PlaceRect& placed,
    const place::PlaceRect& anchor,
    const std::vector<place::PlaceRect>& others,
    const place::PlaceRect& room,
    int32_t gap,
    int32_t height,
    int32_t most_width
) {
    check(inside(placed, room), "a seat button leaves the frame");
    check(placed.x == anchor.x + anchor.width + gap, "a seat button left its anchor's right edge");
    check(
        placed.y == anchor.y + (anchor.height - height) / 2, "a seat button is not centred on Energy"
    );
    check(placed.height == height, "a seat button is not the asked height");
    int32_t limit = most_width;
    for (const place::PlaceRect& other : others) {
        if (other.width <= 0 || other.height <= 0)
            continue;
        const bool shares = other.y < placed.y + placed.height && placed.y < other.y + other.height;
        if (!shares || other.x <= placed.x)
            continue;
        const int32_t room_for = other.x - gap - placed.x;
        if (room_for < limit)
            limit = room_for;
    }
    check(placed.width == limit, "a seat button is not as wide as the room beside Energy");
    check(!meet(placed, anchor), "a seat button meets Energy");
    for (const place::PlaceRect& other : others)
        check(!meet(placed, other), "a seat button meets a gadget");
}

void place_beside_rows() {
    const place::PlaceRect room = rect(0, 0, 640, 480);
    for (const int32_t slots : {2, 3, 10}) {
        const std::vector<place::PlaceRect> gadgets = skirmish_rows(slots);
        const int32_t row_count = (static_cast<int32_t>(gadgets.size()) - 11) / 6;
        check(row_count == slots, "the rows were not built");
        for (int32_t row = 0; row < slots; ++row) {
            const place::PlaceRect anchor = gadgets[static_cast<std::size_t>(11 + row * 6 + 5)];
            std::vector<place::PlaceRect> others;
            others.reserve(gadgets.size() - 1);
            for (std::size_t gadget_at = 0; gadget_at < gadgets.size(); ++gadget_at) {
                if (gadget_at == static_cast<std::size_t>(11 + row * 6 + 5))
                    continue;
                others.push_back(gadgets[gadget_at]);
            }
            const std::optional<place::PlaceRect> placed =
                place::place_beside(anchor, others, room, 4, 17, 48, 160);
            check(placed.has_value(), "a row has no seat button");
            check(placed->x == 386 && placed->x + placed->width == 472, "the button is not 386 to 472");
            expect_beside(*placed, anchor, others, room, 4, 17, 160);
        }
    }

    const place::PlaceRect wide = rect(0, 0, 960, 720);
    for (const int32_t slots : {2, 3, 10}) {
        std::vector<place::PlaceRect> gadgets = skirmish_rows(slots);
        for (place::PlaceRect& gadget : gadgets)
            gadget = scale_rect(gadget);
        for (int32_t row = 0; row < slots; ++row) {
            const place::PlaceRect anchor = gadgets[static_cast<std::size_t>(11 + row * 6 + 5)];
            std::vector<place::PlaceRect> others;
            for (std::size_t gadget_at = 0; gadget_at < gadgets.size(); ++gadget_at) {
                if (gadget_at == static_cast<std::size_t>(11 + row * 6 + 5))
                    continue;
                others.push_back(gadgets[gadget_at]);
            }
            const std::optional<place::PlaceRect> placed =
                place::place_beside(anchor, others, wide, 6, 25, 72, 240);
            check(placed.has_value(), "a row at one and a half has no seat button");
            expect_beside(*placed, anchor, others, wide, 6, 25, 240);
        }
    }

    const place::PlaceRect energy = rect(337, 169, 45, 20);
    const std::vector<place::PlaceRect> shortening{rect(450, 169, 20, 20)};
    const std::optional<place::PlaceRect> shortened =
        place::place_beside(energy, shortening, room, 4, 17, 48, 160);
    check(shortened.has_value(), "a rectangle in the way refused the button");
    check(shortened->x == 386 && shortened->width == 60, "a rectangle in the way did not shorten it");

    const std::vector<place::PlaceRect> tight{rect(430, 169, 20, 20)};
    check(!place::place_beside(energy, tight, room, 4, 17, 48, 160), "too little room gave a button");
    const place::PlaceRect narrow = rect(0, 0, 420, 480);
    check(
        !place::place_beside(energy, {}, narrow, 4, 17, 48, 160),
        "a button that leaves the frame was kept"
    );
    const std::vector<place::PlaceRect> overlapping{rect(370, 169, 30, 20)};
    check(
        !place::place_beside(energy, overlapping, room, 4, 17, 48, 160),
        "a button that meets a gadget was kept"
    );
}

} // namespace

int main() {
    try {
        screen_tests();
        shorten_test();
        drop_test();
        no_room_test();
        beside_test();
        focus_test();
        place_beside_rows();
        column_test();
        middle_test();
        over_test();
        one_width_matches();
        two_button_row();
        std::cout << "oa button place passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
