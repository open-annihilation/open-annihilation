// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// ui.whiteboard's marks, records and batches: each edit applied here and
// queued, the batches taken (texts alone, others up to the byte limit), a
// received batch applied and echoed, the erase squares and marker hits.
#include "oa/ui/hud/whiteboard.hpp"

#include "oa/present/colour_vision/colour_vision.hpp"
#include "oa/ui/hud/shared_views.hpp"

#include "check.hpp"
#include "fixtures.hpp"

#include <string>
#include <vector>

using namespace oa;
using namespace oa::ui::hud;

namespace {

void records() {
    Whiteboard board;
    whiteboard_draw_line(board, 300, 0x1234, 310, 20, 212);
    CHECK(board.lines.size() == 1 && board.outgoing.size() == 1);
    const std::vector<uint8_t> line{1, 0, 0x2c, 0x01, 0x34, 0x12, 212, 0, 0x36, 0x01, 20, 0};
    CHECK(board.outgoing[0] == line);
    whiteboard_place_marker(board, 5, 6, 80, "go");
    const std::vector<uint8_t> marker{3, 0, 5, 0, 6, 0, 80, 'g', 'o', 0};
    CHECK(board.outgoing[1] == marker);
    CHECK(whiteboard_edit_marker(board, 5, 6, "hi"));
    CHECK(!whiteboard_edit_marker(board, 7, 6, "no"));
    const std::vector<uint8_t> edit{5, 0, 5, 0, 6, 0, 'h', 'i', 0};
    CHECK(board.outgoing[2] == edit && board.markers[0].text == "hi");
    CHECK(whiteboard_move_marker(board, 5, 6, 40, 41));
    const std::vector<uint8_t> move{6, 0, 5, 0, 6, 0, 80, 0, 40, 0, 41, 0};
    CHECK(board.outgoing[3] == move && board.markers[0].x == 40);
    whiteboard_spot_erase(board, 100, 200);
    const std::vector<uint8_t> spot{10, 0, 100, 0, 200, 0};
    CHECK(board.outgoing[4] == spot);
}

void batches() {
    Whiteboard board;
    // A line, then a marker: the line goes first, then the marker alone.
    whiteboard_draw_line(board, 0, 0, 1, 1, 1);
    whiteboard_place_marker(board, 2, 2, 1, "");
    whiteboard_draw_line(board, 0, 0, 1, 1, 1);
    auto batch = whiteboard_take_batch(board);
    CHECK(batch.size() == 13 && batch[0] == 1 && batch[1] == 1);
    batch = whiteboard_take_batch(board);
    CHECK(batch.size() == 1 + 8 && batch[0] == 1 && batch[1] == 3);
    batch = whiteboard_take_batch(board);
    CHECK(batch[0] == 1 && board.outgoing.empty());
    CHECK(whiteboard_take_batch(board).empty());
    // Lines fill a batch while 12 more bytes stay under 99: eight of them.
    for (int i = 0; i < 10; ++i)
        whiteboard_draw_line(board, i, 0, i, 1, 1);
    batch = whiteboard_take_batch(board);
    CHECK(batch[0] == 8 && batch.size() == 97 && board.outgoing.size() == 2);
}

void receiving() {
    hud_test::TestWorld w;
    w.add_player(1, 2);
    w.world->player_info[1].color = 2;
    Whiteboard sent;
    whiteboard_place_marker(sent, 70, 80, kPlayerDotColors[2], "attack here");
    whiteboard_draw_line(sent, 1, 2, 3, 4, kPlayerDotColors[2]);
    Whiteboard board;
    std::vector<std::string> echoed;
    WhiteboardEcho echo{};
    echo.user = &echoed;
    echo.marker = [](void* user, uint8_t, const char* text) {
        static_cast<std::vector<std::string>*>(user)->emplace_back(text);
    };
    auto batch = whiteboard_take_batch(sent);
    CHECK(whiteboard_apply_batch(board, batch, echo) == 1);
    CHECK(board.markers.size() == 1 && board.markers[0].text == "attack here");
    CHECK(board.received_marker && board.received_x == 70 && board.received_y == 80);
    CHECK(echoed.size() == 1 && echoed[0] == "attack here");
    batch = whiteboard_take_batch(sent);
    CHECK(whiteboard_apply_batch(board, batch, echo) == 1 && board.lines.size() == 1);
    // A batch cut short applies its whole records only.
    whiteboard_draw_line(sent, 9, 9, 9, 9, 1);
    whiteboard_draw_line(sent, 8, 8, 8, 8, 1);
    batch = whiteboard_take_batch(sent);
    batch.resize(batch.size() - 3);
    CHECK(whiteboard_apply_batch(board, batch, echo) == 1 && board.lines.size() == 2);
    CHECK(
        whiteboard_echo_line(*w.world, kPlayerDotColors[2], "attack here") ==
        "*Player 1: attack here"
    );
    CHECK(
        whiteboard_echo_line(*w.world, kPlayerDotColors[2], "") == "*Player 1 added a new marker"
    );
    CHECK(whiteboard_echo_line(*w.world, 3, "x").empty());
}

void recorded_stock_colour() {
    namespace vision = oa::present::colour_vision;
    hud_test::TestWorld w;
    w.add_player(0, 0);
    w.world->player_info[0].color = 0;
    vision::set_active(vision::Mode::protanopia, false);
    Whiteboard sent;
    whiteboard_place_marker(sent, 4, 5, stock_dot_color(*w.world, 0), "");
    const auto batch = whiteboard_take_batch(sent);
    // Count, then the marker: type, pad, x, y, the recorded colour, no text.
    CHECK(batch.size() == 1 + 8 && batch[7] == 227);
    CHECK(whiteboard_echo_line(*w.world, 227, "mark") == "*Player 0: mark");
    CHECK(drawn_dot_color(227) == vision::player_dot(0));
    CHECK(drawn_dot_color(1) == 1);
    vision::set_active(vision::Mode::off, false);
    CHECK(drawn_dot_color(227) == 227);
}

void erasing() {
    Whiteboard board;
    whiteboard_draw_line(board, 100, 100, 400, 400, 1); // anchored at its first end
    whiteboard_draw_line(board, 400, 400, 100, 100, 1);
    whiteboard_place_marker(board, 109, 100, 1, "");
    whiteboard_place_marker(board, 110, 100, 1, "");
    CHECK(whiteboard_marker_at(board, 100, 100) == 0);
    CHECK(whiteboard_marker_at(board, 105, 100) == 1);
    whiteboard_spot_erase(board, 100, 100);
    // The marker at 110 is past the square's far edge.
    CHECK(board.lines.size() == 1 && board.markers.size() == 1 && board.markers[0].x == 110);
    whiteboard_area_erase(board, 360, 360);
    CHECK(board.lines.empty());
}

} // namespace

int main() {
    records();
    batches();
    receiving();
    recorded_stock_colour();
    erasing();
    std::puts("ui-hud-whiteboard-test: ok");
    return 0;
}
