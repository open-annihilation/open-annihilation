// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// ui.resource-panel over a small World: the rows a watcher and a player
// list, a replay's seats, the amount, bar and income formats, the drawn row,
// its square in the player's dot colour, the hits, the F4
// cycle with the kills board, a watcher's view switches, and the clock,
// wind and tidal line and where it goes: the top bar's two sections, beside
// each other in the top bar, or the battlefield.
#include "oa/ui/hud/resource_panel.hpp"

#include "check.hpp"
#include "fixtures.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace oa;
using namespace oa::ui::hud;

namespace {

struct Drawn {
    struct Fill {
        int32_t x{}, y{}, width{}, height{};
        uint8_t color{};
    };

    struct Text {
        int32_t x{}, y{};
        std::string text;
        uint8_t color{};
    };

    std::vector<Fill> fills;
    std::vector<Text> texts;

    ResourcePanelSink sink() {
        ResourcePanelSink s{};
        s.user = this;
        s.fill = [](void* user, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t c) {
            static_cast<Drawn*>(user)->fills.push_back({x, y, w, h, c});
        };
        s.text = [](void* user, int32_t x, int32_t y, const char* text, uint8_t c) {
            static_cast<Drawn*>(user)->texts.push_back({x, y, text, c});
        };
        return s;
    }

    [[nodiscard]] const Text* find(const char* text) const {
        for (const auto& t : texts)
            if (t.text == text)
                return &t;
        return nullptr;
    }
};

void rows() {
    hud_test::TestWorld w;
    w.add_player(0, 1);
    w.add_player(2, 2);
    w.add_player(5, 2);
    SharedPlayerViews shared{};
    // A player lists only the slots that share their position.
    auto listed = resource_panel_rows(*w.world, false, shared);
    CHECK(listed.count == 0 && !listed.own_view_row && resource_panel_height(listed) == 0);
    shared.shares_position[5] = true;
    shared.shares_position[0] = true; // slot 0 is never listed
    listed = resource_panel_rows(*w.world, false, shared);
    CHECK(listed.count == 1 && listed.slots[0] == 5);
    // A watcher lists every named slot from 1, and its own-view row.
    listed = resource_panel_rows(*w.world, true, {});
    CHECK(listed.count == 2 && listed.slots[0] == 2 && listed.slots[1] == 5);
    CHECK(listed.own_view_row && resource_panel_height(listed) == 90);
    // Local player 0 watches when its setup says so.
    CHECK(!local_player_watches(*w.world));
    w.world->player_info[0].options = OA_SETUP_OPTION_WATCHER;
    CHECK(local_player_watches(*w.world));
}

void formats() {
    char text[32];
    format_panel_amount(text, sizeof text, 9999.4F);
    CHECK(std::strcmp(text, "9999") == 0);
    format_panel_amount(text, sizeof text, 10000.0F);
    CHECK(std::strcmp(text, "10.0K") == 0);
    format_panel_amount(text, sizeof text, 99949.0F);
    CHECK(std::strcmp(text, "99.9K") == 0);
    format_panel_amount(text, sizeof text, 250000.0F);
    CHECK(std::strcmp(text, "250K") == 0);
    CHECK(panel_bar_pixels(500.0F, 1000.0F) == 50);
    CHECK(panel_bar_pixels(1000.0F, 1000.0F) == 99);
    CHECK(panel_bar_pixels(5.0F, 0.0F) == 0);
    CHECK(panel_bar_pixels(-5.0F, 100.0F) == 0);
    CHECK(panel_bar_pixels(1.99F, 100.0F) == 1);
    format_panel_income(text, sizeof text, 3.25F, true);
    CHECK(std::strcmp(text, "+3.2") == 0 || std::strcmp(text, "+3.3") == 0);
    format_panel_income(text, sizeof text, 120.6F, false);
    CHECK(std::strcmp(text, "+121") == 0);
}

void drawing() {
    hud_test::TestWorld w;
    auto& player = w.add_player(3, 2);
    w.world->player_info[3].color = 7;
    player.metal = 250.0F;
    player.metal_storage = 1000.0F;
    player.energy = 12500.0F;
    player.energy_storage = 25000.0F;
    player.metal_produced = 1.5F;
    player.energy_produced = 42.0F;
    SharedPlayerViews shared{};
    shared.shares_position[3] = true;
    const auto listed = resource_panel_rows(*w.world, false, shared);
    ResourcePanel panel{};
    panel.x = 400;
    panel.y = 50;
    Drawn drawn;
    draw_resource_panel(*w.world, panel, listed, drawn.sink());
    const auto* name = drawn.find("Player 3");
    CHECK(name != nullptr && name->x == 400 + 0x2c && name->y == 50 && name->color == 0x51);
    // Stored metal right-aligned in five characters: "250" starts two in.
    const auto* metal = drawn.find("250");
    CHECK(metal != nullptr && metal->x == 400 + 16 && metal->y == 60);
    const auto* energy = drawn.find("12.5K");
    CHECK(energy != nullptr && energy->x == 400 && energy->y == 70);
    CHECK(drawn.find("+1.5") != nullptr && drawn.find("+1.5")->x == 400 + 0x95);
    CHECK(drawn.find("+42") != nullptr && drawn.find("+42")->y == 70);
    // The colour square in the player's dot colour, not the colour's number,
    // then each bar's edges, trough and fill.
    CHECK(drawn.fills[0].x == 400 + 0x24 && drawn.fills[0].y == 51);
    CHECK(drawn.fills[0].color == kPlayerDotColors[7] && listed.colours[0] == kPlayerDotColors[7]);
    bool metal_fill = false, energy_fill = false;
    for (const auto& fill : drawn.fills) {
        metal_fill |= fill.color == kResourcePanelMetalBarColor && fill.width == 25 && fill.y == 60;
        energy_fill |=
            fill.color == kResourcePanelEnergyBarColor && fill.width == 50 && fill.y == 70;
    }
    CHECK(metal_fill && energy_fill);
    // The viewed and locked-on rows are highlighted, the lock flashing.
    panel.viewed_slot = 3;
    drawn = {};
    draw_resource_panel(*w.world, panel, listed, drawn.sink());
    CHECK(drawn.fills[0].color == kResourcePanelViewRowColor && drawn.fills[0].width == 210);
    panel.locked_slot = 3;
    drawn = {};
    draw_resource_panel(*w.world, panel, listed, drawn.sink());
    const uint8_t first = drawn.fills[0].color;
    drawn = {};
    draw_resource_panel(*w.world, panel, listed, drawn.sink());
    CHECK(first != drawn.fills[0].color && (first ^ drawn.fills[0].color) == 0x20);
}

void replay_seats() {
    hud_test::TestWorld w;
    // A replay seats its recorded players from slot 0 and its watcher after
    // them. The panel lists the seats it is given, in their order, the
    // first slot included, and never the watcher's.
    auto& hoster = w.add_player(0, 3);
    hoster.metal = 1000.0F;
    hoster.metal_storage = 1000.0F;
    hoster.energy = 500.0F;
    hoster.energy_storage = 1000.0F;
    hoster.metal_produced = 0.0F;
    hoster.energy_produced = 0.0F;
    w.add_player(1, 3);
    auto& joiner = w.add_player(2, 3);
    joiner.metal_storage = 1000.0F;
    joiner.energy_storage = 1000.0F;
    w.add_player(3, 2);
    w.world->player_info[3].options = OA_SETUP_OPTION_WATCHER;
    std::array<ResourcePanelSeat, 3> seats{};
    seats[0].slot = 2;
    seats[0].colour = kPlayerDotColors[1];
    seats[0].economy = true;
    seats[1].slot = 0;
    seats[1].colour = kPlayerDotColors[0];
    seats[1].economy = true;
    seats[1].income = true;
    seats[1].metal_income = 1.0F;
    seats[1].energy_income = 75.0F;
    seats[2].slot = 1;
    seats[2].colour = kPlayerDotColors[2];
    const auto listed = resource_panel_seat_rows(*w.world, seats);
    CHECK(listed.count == 3 && !listed.own_view_row && resource_panel_height(listed) == 90);
    CHECK(listed.slots[0] == 2 && listed.slots[1] == 0 && listed.slots[2] == 1);
    CHECK(listed.colours[0] == kPlayerDotColors[1] && listed.colours[2] == kPlayerDotColors[2]);
    // Stored amounts are the slot's; the income is the recording's.
    CHECK(listed.figures[1].has_value() && listed.figures[1]->metal == 1000.0F);
    CHECK(listed.figures[1]->metal_income == 1.0F && listed.figures[1]->energy_income == 75.0F);
    // A computer whose economy the recording never gave shows no figures.
    CHECK(!listed.figures[2].has_value());
    ResourcePanel panel{};
    panel.x = 0;
    panel.y = 0;
    Drawn drawn;
    draw_resource_panel(*w.world, panel, listed, drawn.sink());
    CHECK(drawn.find("Player 2") != nullptr && drawn.find("Player 2")->y == 0);
    CHECK(drawn.find("Player 0") != nullptr && drawn.find("Player 0")->y == 30);
    CHECK(drawn.find("Player 1") != nullptr && drawn.find("Player 1")->y == 60);
    CHECK(drawn.find("Player 3") == nullptr && drawn.find(kResourcePanelOwnViewRow) == nullptr);
    CHECK(drawn.find("+1.0") != nullptr && drawn.find("+1.0")->y == 40);
    CHECK(drawn.find("+75") != nullptr && drawn.find("+75")->y == 50);
    // The silent computer's row draws its square and name, and nothing past them.
    int below_computer = 0;
    for (const auto& text : drawn.texts)
        below_computer += text.y > 60 ? 1 : 0;
    CHECK(below_computer == 0);
    // A seat that has gone keeps its row, without figures.
    seats[0].gone = true;
    const auto gone = resource_panel_seat_rows(*w.world, seats);
    CHECK(gone.count == 3 && gone.slots[0] == 2 && !gone.figures[0].has_value());
}

void hits_and_switches() {
    hud_test::TestWorld w;
    w.add_player(1, 2);
    w.add_player(4, 2);
    auto listed = resource_panel_rows(*w.world, true, {});
    ResourcePanel panel{};
    panel.x = 100;
    panel.y = 50;
    CHECK(!resource_panel_hit(panel, listed, 50, 60).inside);
    auto hit = resource_panel_hit(panel, listed, 150, 60);
    CHECK(hit.inside && hit.row == 0 && !hit.own_view);
    hit = resource_panel_hit(panel, listed, 150, 95);
    CHECK(hit.row == 1);
    CHECK(resource_panel_hit(panel, listed, 150, 125).own_view);
    // A player is never switched.
    CHECK(resource_panel_view_switch(panel, listed, hit, false) == ViewSwitch::none);
    // Row 2 stands for slot 2, though it shows slot 4.
    CHECK(resource_panel_view_switch(panel, listed, hit, true) == ViewSwitch::view_player);
    CHECK(panel.viewed_slot == 2 && panel.locked_slot == 0);
    CHECK(resource_panel_view_switch(panel, listed, hit, true) == ViewSwitch::lock_camera);
    CHECK(panel.locked_slot == 2);
    CHECK(resource_panel_view_switch(panel, listed, hit, true) == ViewSwitch::unlock_camera);
    CHECK(panel.locked_slot == 0 && panel.viewed_slot == 2);
    // With a lock, viewing another row moves the lock to it.
    panel.locked_slot = 2;
    hit = resource_panel_hit(panel, listed, 150, 60);
    CHECK(resource_panel_view_switch(panel, listed, hit, true) == ViewSwitch::view_player);
    CHECK(panel.viewed_slot == 1 && panel.locked_slot == 1);
    hit = resource_panel_hit(panel, listed, 150, 125);
    CHECK(resource_panel_view_switch(panel, listed, hit, true) == ViewSwitch::own_view);
    CHECK(panel.viewed_slot == 0 && panel.locked_slot == 0);
}

void f4_cycle() {
    ResourcePanel panel{};
    // Panel out, board away: the panel goes.
    CHECK(resource_panel_f4(panel, false, true) && panel.hidden);
    // Panel away, board away: the board takes the key.
    CHECK(!resource_panel_f4(panel, false, true));
    // Panel away, board out: the panel comes back.
    CHECK(resource_panel_f4(panel, true, true) && !panel.hidden);
    // Panel out, board out: the board takes the key.
    CHECK(!resource_panel_f4(panel, true, true));
    // Without rows the key is the board's.
    CHECK(!resource_panel_f4(panel, false, false) && !panel.hidden);
}

void clock_line() {
    char text[64];
    format_game_time(text, sizeof text, 30 * (3600 + 2 * 60 + 5) + 29);
    CHECK(std::strcmp(text, "Game Time : 01:02:05") == 0);
    CHECK(wind_readout(1000, 0, 3000, 0, 30).current == 0);
    const auto wind = wind_readout(1000, 0, 3000, 2400, kDefaultWindGenerator);
    // (1000 * 30 + 1200) / 2400 = 13; the most is capped at the output.
    CHECK(wind.current == 13 && wind.minimum == 0 && wind.maximum == 30);
    format_wind(text, sizeof text, wind, false);
    CHECK(std::strcmp(text, "Wind : +13 (0-30)") == 0);
    format_wind(text, sizeof text, wind, true);
    CHECK(std::strcmp(text, "Wind : (0-30)") == 0);
    format_tidal(text, sizeof text, 19.9F);
    CHECK(std::strcmp(text, "Tidal : +19") == 0);
}

void clock_line_place() {
    // The top bar past PANELTOP at column 642: PANELBOT pieces of 513
    // columns, three sections of 171 each. The parts' widths: the longer
    // label 34, the figures 50, the time's line 103, "Game Time" 46 and the
    // time alone 40.
    const TopBarPieces pieces{642, 513};
    const ClockLineWidths widths{34, 50, 103, 46, 40};
    // 1920x1080: the bar ends at column 960 and holds both sections. The
    // labels end 16 columns plus the longer label into the first; the time
    // starts 16 columns into the second, at 813.
    const auto wide = place_clock_line(pieces, 960, widths, 1920, true);
    CHECK(wide.spot == ClockLineSpot::sections);
    CHECK(wide.figures_x == 692);
    CHECK(wide.time_x == 829);
    // 1280x720: the bar ends at column 854, inside the second section; the
    // time goes beside the figures, 12 columns past them.
    const auto one = place_clock_line(pieces, 854, widths, 1280, true);
    CHECK(one.spot == ClockLineSpot::beside);
    CHECK(one.figures_x == 692);
    CHECK(one.time_x == 754);
    // 1024x768: the battlefield, whatever the bar holds.
    CHECK(place_clock_line(pieces, 640, widths, 1024, true).spot == ClockLineSpot::battlefield);
    CHECK(place_clock_line(pieces, 960, widths, 1024, true).spot == ClockLineSpot::battlefield);
    // A bar with no room past PANELTOP keeps the line on the battlefield.
    CHECK(place_clock_line(pieces, 640, widths, 1280, true).spot == ClockLineSpot::battlefield);
    // On the battlefield, the line keeps its place at the top left.
    CHECK(kClockLineLeft == 130 && kClockLineTop == 34 && kClockLineStep == 10);
    const auto rows = clock_line_battlefield_rows(true);
    CHECK(rows.time == 34 && rows.wind == 44 && rows.tidal == 54);

    // While the console's Clock shows the game time, the line leaves it
    // out: the wind over the tidal strength in the first section, with no
    // place for the time, wherever the bar reaches past their figures, on
    // a window 1024 pixels wide or narrower too (a bar of the original
    // game's size at 1024x768).
    const auto first = place_clock_line(pieces, 960, widths, 1920, false);
    CHECK(first.spot == ClockLineSpot::first_section);
    CHECK(first.figures_x == 692 && first.time_x == 0);
    CHECK(place_clock_line(pieces, 742, widths, 1280, false).spot == ClockLineSpot::first_section);
    CHECK(place_clock_line(pieces, 741, widths, 1280, false).spot == ClockLineSpot::battlefield);
    const auto narrow = place_clock_line(pieces, 1024, widths, 1024, false);
    CHECK(narrow.spot == ClockLineSpot::first_section && narrow.figures_x == 692);
    // A bar that ends at its readouts, as at 640x480, leaves it on the
    // battlefield, where the wind and the tidal strength take the first two
    // lines, with no line for the time.
    CHECK(place_clock_line(pieces, 640, widths, 640, false).spot == ClockLineSpot::battlefield);
    const auto without_time = clock_line_battlefield_rows(false);
    CHECK(!without_time.time && without_time.wind == 34 && without_time.tidal == 44);
    // A line is cut into its label, signed amount and the rest.
    const auto wind = split_clock_line("Wind : +26 (24-30)");
    CHECK(wind.label == "Wind : " && wind.amount == "+26" && wind.rest == " (24-30)");
    const auto watched = split_clock_line("Wind : (24-30)");
    CHECK(watched.label == "Wind : " && watched.amount.empty() && watched.rest == "(24-30)");
    const auto time = split_clock_line("Game Time : 00:00:02");
    CHECK(time.label == "Game Time : " && time.amount.empty() && time.rest == "00:00:02");
    CHECK(split_clock_line("Tidal").label == "Tidal");
}

// The scale of the chrome on a window of a size, with the top bar's
// pieces and the parts' widths of clock_line_place.
double chrome_scale(int32_t width, int32_t height, bool game_time) {
    const TopBarPieces pieces{642, 513};
    const ClockLineWidths widths{34, 50, 103, 46, 40};
    // The interface's own scale, and the column just past its top bar's
    // last: the window's width in source columns.
    const double scale = std::min({width / 640.0, height / 480.0, 2.0});
    const auto bar_end = static_cast<int32_t>(std::ceil(width / scale - 1e-9));
    return clock_line_chrome_scale(pieces, widths, game_time, width, bar_end, scale);
}

void clock_line_room() {
    const TopBarPieces pieces{642, 513};
    const ClockLineWidths widths{34, 50, 103, 46, 40};
    // The time 16 columns into the second section; "Game Time" over the
    // time 12 past the figures; the figures alone.
    CHECK(clock_line_bar_end(pieces, widths, ClockLineSpot::sections) == 932);
    CHECK(clock_line_bar_end(pieces, widths, ClockLineSpot::beside) == 800);
    CHECK(clock_line_bar_end(pieces, widths, ClockLineSpot::first_section) == 742);
    CHECK(clock_line_bar_end(pieces, widths, ClockLineSpot::battlefield) == 0);
    // Figures past the first section leave no room for two sections.
    CHECK(clock_line_bar_end(pieces, {34, 130, 103, 46, 40}, ClockLineSpot::sections) == 0);
    CHECK(clock_line_bar_end({642, 0}, widths, ClockLineSpot::beside) == 0);

    const auto near = [](double scale, double expected) {
        return std::abs(scale - expected) < 1e-9;
    };
    // 5:4 and 4:3, whose bars end at their readouts: drawn smaller, as far
    // as one section past the readouts needs (816 columns), since two
    // (948) would take them under one and a half times the interface.
    CHECK(near(chrome_scale(1280, 1024, true), 1280.0 / 816));
    CHECK(near(chrome_scale(1152, 864, true), 1152.0 / 816));
    // 16:10 at 1440x900 has room for two sections at that scale or more.
    CHECK(near(chrome_scale(1440, 900, true), 1440.0 / 948));
    // Bars that reach far enough keep the interface's scale: two sections
    // at 1920x1080, one at 1280x720 (which two would take under 1.5).
    CHECK(near(chrome_scale(1920, 1080, true), 2.0));
    CHECK(near(chrome_scale(1280, 720, true), 1.5));
    // At 1024 pixels wide or narrower the line stays on the battlefield.
    CHECK(near(chrome_scale(1024, 768, true), 1.6));
    // Without the game time the figures alone need room (758 columns), or
    // none past the bar's end at 1440x900; at 1024 pixels wide or narrower
    // the chrome keeps its scale, the line going in the bar only where it
    // already reaches past the figures.
    CHECK(near(chrome_scale(1280, 1024, false), 1280.0 / 758));
    CHECK(near(chrome_scale(1440, 900, false), 1.875));
    CHECK(near(chrome_scale(1024, 768, false), 1.6));
}

} // namespace

int main() {
    rows();
    formats();
    drawing();
    replay_seats();
    hits_and_switches();
    f4_cycle();
    clock_line();
    clock_line_place();
    clock_line_room();
    std::puts("ui-hud-resource-panel-test: ok");
    return 0;
}
