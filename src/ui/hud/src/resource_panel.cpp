// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/hud/resource_panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace oa::ui::hud {
namespace {

/// Below this a stored amount is shown whole.
constexpr float kAmountThousandsFrom = 10000.0F;
/// From this on a stored amount is shown in whole thousands.
constexpr float kAmountWholeThousandsFrom = 100000.0F;
constexpr float kThousand = 1000.0F;
constexpr float kBarScale = 100.0F;
constexpr int32_t kBarFullest = kResourcePanelBarWidth - 1;
/// Rows of a storage bar, below its row's text line.
constexpr int32_t kBarRows = 3;
constexpr int32_t kMetalLineY = 10;
constexpr int32_t kEnergyLineY = 20;
constexpr int32_t kSquareSide = 8;
constexpr uint32_t kTicksPerSecond = 30;
constexpr uint32_t kSecondsPerMinute = 60;
constexpr uint32_t kSecondsPerHour = 3600;
/// Setup option of a watcher (PlayerSetupInfo.options).
constexpr uint32_t kWatcherOption = OA_SETUP_OPTION_WATCHER;

bool slot_in_use(const Player& player) noexcept {
    return player.in_use != 0;
}

/// A slot's own stored amounts, storage and incomes.
ResourcePanelFigures figures_of(const Player& player) noexcept {
    return {
        player.metal,
        player.metal_storage,
        player.metal_produced,
        player.energy,
        player.energy_storage,
        player.energy_produced
    };
}

/// Copies a player's name, which need not end in a NUL.
void copy_name(char* out, std::size_t size, const Player& player) {
    std::snprintf(out, size, "%.*s", static_cast<int>(sizeof player.name), player.name);
}

/// Draws a storage bar: its trough, the black edges a background-less panel
/// gives it, and its fill.
void draw_bar(
    const ResourcePanel& panel,
    const ResourcePanelSink& sink,
    int32_t x,
    int32_t y,
    int32_t pixels,
    uint8_t color
) {
    if (sink.fill == nullptr)
        return;
    if (panel.background == ResourcePanelBackground::none) {
        sink.fill(sink.user, x, y - 1, kResourcePanelBarWidth, 1, kResourcePanelTroughColor);
        sink.fill(sink.user, x, y + kBarRows, kResourcePanelBarWidth, 1, kResourcePanelTroughColor);
    }
    sink.fill(sink.user, x, y, kResourcePanelBarWidth, kBarRows, kResourcePanelTroughColor);
    if (pixels > 0)
        sink.fill(sink.user, x, y, pixels, kBarRows, color);
}

/// Draws a stored amount right-aligned in kResourcePanelAmountChars characters.
void draw_amount(const ResourcePanelSink& sink, int32_t x, int32_t y, float amount) {
    char text[32];
    format_panel_amount(text, sizeof text, amount);
    const auto length = static_cast<int32_t>(std::strlen(text));
    if (sink.text != nullptr)
        sink.text(
            sink.user,
            x + kResourcePanelCharWidth * (kResourcePanelAmountChars - length),
            y,
            text,
            kResourcePanelTextColor
        );
}

/// The energy of one wind speed: rounded to nearest over the divisor, at most the output.
int32_t wind_energy(int32_t speed, int32_t generator, int32_t divisor) noexcept {
    const int32_t half = divisor / 2;
    const int32_t energy = (speed * generator + half) / divisor;
    return energy < generator ? energy : generator;
}

} // namespace

bool local_player_watches(const World& world) noexcept {
    const Player* local = world_player(const_cast<World*>(&world), world.game.local_player_index);
    if (local == nullptr)
        return false;
    const PlayerSetupInfo* info = world_player_info(&world, local);
    return info != nullptr && (info->options & kWatcherOption) != 0;
}

ResourcePanelRows
resource_panel_rows(const World& world, bool watching, const SharedPlayerViews& shared) noexcept {
    ResourcePanelRows rows{};
    for (uint8_t slot = kResourcePanelFirstSlot; slot < kResourcePanelSlotEnd; ++slot) {
        const Player& player = world.game.players[slot];
        const bool listed =
            watching ? slot_in_use(player) && player.name[0] != '\0' : shared.shares_position[slot];
        if (!listed)
            continue;
        rows.slots[rows.count] = slot;
        rows.colours[rows.count] = player_dot_color(world, slot);
        rows.figures[rows.count] = figures_of(player);
        ++rows.count;
    }
    rows.own_view_row = watching;
    return rows;
}

ResourcePanelRows
resource_panel_seat_rows(const World& world, std::span<const ResourcePanelSeat> seats) noexcept {
    ResourcePanelRows rows{};
    for (const ResourcePanelSeat& seat : seats) {
        if (rows.count >= OA_PLAYER_COUNT)
            break;
        if (seat.slot >= OA_PLAYER_COUNT)
            continue;
        rows.slots[rows.count] = seat.slot;
        rows.colours[rows.count] = seat.colour;
        if (seat.economy && !seat.gone) {
            ResourcePanelFigures figures = figures_of(world.game.players[seat.slot]);
            if (seat.income) {
                figures.metal_income = seat.metal_income;
                figures.energy_income = seat.energy_income;
            }
            rows.figures[rows.count] = figures;
        }
        ++rows.count;
    }
    return rows;
}

void format_panel_amount(char* out, std::size_t size, float amount) {
    if (kAmountThousandsFrom > amount)
        std::snprintf(out, size, "%.0f", static_cast<double>(amount));
    else if (kAmountWholeThousandsFrom > amount)
        std::snprintf(out, size, "%.1fK", static_cast<double>(amount / kThousand));
    else
        std::snprintf(out, size, "%.0fK", static_cast<double>(amount / kThousand));
}

int32_t panel_bar_pixels(float stored, float capacity) noexcept {
    if (capacity == 0.0F || std::isnan(capacity))
        return 0;
    const float share = stored / capacity * kBarScale;
    if (!(share >= 0.0F))
        return 0;
    if (share >= static_cast<float>(kBarFullest))
        return kBarFullest;
    return static_cast<int32_t>(share);
}

void format_panel_income(char* out, std::size_t size, float income, bool metal) {
    std::snprintf(out, size, metal ? "+%.1f" : "+%.0f", static_cast<double>(income));
}

int32_t resource_panel_height(const ResourcePanelRows& rows) noexcept {
    return kResourcePanelRowHeight * (rows.count + (rows.own_view_row ? 1 : 0));
}

void draw_resource_panel(
    const World& world,
    ResourcePanel& panel,
    const ResourcePanelRows& rows,
    const ResourcePanelSink& sink
) {
    panel.flash = static_cast<uint8_t>(panel.flash ^ kResourcePanelLockFlash);
    const int32_t height = resource_panel_height(rows);
    if (height == 0)
        return;
    if (panel.background == ResourcePanelBackground::solid && sink.fill != nullptr)
        sink.fill(
            sink.user, panel.x, panel.y, kResourcePanelWidth, height, kResourcePanelTroughColor
        );
    for (uint8_t row = 0; row < rows.count; ++row) {
        const uint8_t slot = rows.slots[row];
        const Player& player = world.game.players[slot < OA_PLAYER_COUNT ? slot : 0];
        const int32_t x = panel.x;
        const int32_t y = panel.y + row * kResourcePanelRowHeight;
        if (sink.fill != nullptr) {
            if (panel.locked_slot != 0 && slot == panel.locked_slot)
                sink.fill(
                    sink.user, x, y, kResourcePanelWidth, kResourcePanelRowHeight, panel.flash
                );
            else if (panel.viewed_slot != 0 && slot == panel.viewed_slot)
                sink.fill(
                    sink.user,
                    x,
                    y,
                    kResourcePanelWidth,
                    kResourcePanelRowHeight,
                    kResourcePanelViewRowColor
                );
            sink.fill(
                sink.user,
                x + kResourcePanelSquareX,
                y + 1,
                kSquareSide,
                kSquareSide,
                rows.colours[row]
            );
        }
        char text[64];
        copy_name(text, sizeof text, player);
        if (sink.text != nullptr)
            sink.text(sink.user, x + kResourcePanelBarX, y, text, kResourcePanelNameColor);
        const std::optional<ResourcePanelFigures>& figures = rows.figures[row];
        if (!figures)
            continue;
        draw_amount(sink, x, y + kMetalLineY, figures->metal);
        draw_bar(
            panel,
            sink,
            x + kResourcePanelBarX,
            y + kMetalLineY,
            panel_bar_pixels(figures->metal, figures->metal_storage),
            kResourcePanelMetalBarColor
        );
        format_panel_income(text, sizeof text, figures->metal_income, true);
        if (sink.text != nullptr)
            sink.text(
                sink.user, x + kResourcePanelIncomeX, y + kMetalLineY, text, kResourcePanelTextColor
            );
        draw_amount(sink, x, y + kEnergyLineY, figures->energy);
        draw_bar(
            panel,
            sink,
            x + kResourcePanelBarX,
            y + kEnergyLineY,
            panel_bar_pixels(figures->energy, figures->energy_storage),
            kResourcePanelEnergyBarColor
        );
        format_panel_income(text, sizeof text, figures->energy_income, false);
        if (sink.text != nullptr)
            sink.text(
                sink.user,
                x + kResourcePanelIncomeX,
                y + kEnergyLineY,
                text,
                kResourcePanelTextColor
            );
    }
    if (rows.own_view_row && sink.text != nullptr)
        sink.text(
            sink.user,
            panel.x + kResourcePanelBarX,
            panel.y + rows.count * kResourcePanelRowHeight + kMetalLineY,
            kResourcePanelOwnViewRow,
            kResourcePanelNameColor
        );
}

ResourcePanelHit resource_panel_hit(
    const ResourcePanel& panel, const ResourcePanelRows& rows, int32_t x, int32_t y
) noexcept {
    ResourcePanelHit hit{};
    const int32_t height = resource_panel_height(rows);
    if (x <= panel.x || x >= panel.x + kResourcePanelWidth || y <= panel.y || y >= panel.y + height)
        return hit;
    hit.inside = true;
    // The panel's last row's height belongs to the own-view row while a
    // watcher has it.
    if (rows.own_view_row && y > panel.y + height - kResourcePanelRowHeight) {
        hit.own_view = true;
        return hit;
    }
    hit.row = static_cast<int8_t>((y - panel.y) / kResourcePanelRowHeight);
    return hit;
}

bool resource_panel_f4(ResourcePanel& panel, bool board_out, bool has_rows) noexcept {
    if (!has_rows)
        return false;
    if (panel.hidden && board_out) {
        panel.hidden = false;
        return true;
    }
    if (!panel.hidden && !board_out) {
        panel.hidden = true;
        return true;
    }
    return false;
}

ViewSwitch resource_panel_view_switch(
    ResourcePanel& panel, const ResourcePanelRows& rows, const ResourcePanelHit& hit, bool watching
) noexcept {
    if (!watching || !hit.inside)
        return ViewSwitch::none;
    if (hit.own_view) {
        panel.locked_slot = 0;
        panel.viewed_slot = 0;
        return ViewSwitch::own_view;
    }
    if (hit.row < 0)
        return ViewSwitch::none;
    (void)rows;
    // The row's number from 1 stands for a player slot, whatever slot the
    // row shows.
    const auto slot = static_cast<uint8_t>(hit.row + 1);
    if (slot == panel.locked_slot) {
        panel.locked_slot = 0;
        return ViewSwitch::unlock_camera;
    }
    if (slot == panel.viewed_slot) {
        panel.locked_slot = slot;
        return ViewSwitch::lock_camera;
    }
    panel.viewed_slot = slot;
    if (panel.locked_slot != 0)
        panel.locked_slot = slot;
    return ViewSwitch::view_player;
}

WindReadout wind_readout(
    int32_t strength, int32_t minimum, int32_t maximum, int32_t divisor, int32_t generator
) noexcept {
    WindReadout wind{};
    if (divisor <= 0)
        return wind;
    wind.current = wind_energy(strength, generator, divisor);
    wind.minimum = wind_energy(minimum, generator, divisor);
    wind.maximum = wind_energy(maximum, generator, divisor);
    return wind;
}

void format_game_time(char* out, std::size_t size, uint32_t tick) {
    const uint32_t seconds = tick / kTicksPerSecond;
    std::snprintf(
        out,
        size,
        "Game Time : %02u:%02u:%02u",
        seconds / kSecondsPerHour,
        seconds / kSecondsPerMinute % kSecondsPerMinute,
        seconds % kSecondsPerMinute
    );
}

void format_wind(char* out, std::size_t size, const WindReadout& wind, bool watching) {
    if (watching)
        std::snprintf(out, size, "Wind : (%d-%d)", wind.minimum, wind.maximum);
    else
        std::snprintf(out, size, "Wind : +%d (%d-%d)", wind.current, wind.minimum, wind.maximum);
}

void format_tidal(char* out, std::size_t size, float tidal_strength) {
    std::snprintf(out, size, "Tidal : +%d", static_cast<int>(tidal_strength));
}

ClockLinePlace place_clock_line(
    const TopBarPieces& pieces,
    int32_t bar_end,
    const ClockLineWidths& widths,
    int32_t window_width,
    bool game_time
) noexcept {
    ClockLinePlace place{};
    const int32_t section = pieces.width / kTopBarPieceSections;
    // Without the game time the line needs only the first section, which
    // the bar of a window of any width may hold.
    if (section <= 0 || (game_time && window_width <= kClockLineBattlefieldMaxWidth))
        return place;
    const int32_t second = pieces.left + section;
    const int32_t figures_end = pieces.left + kClockLineInset + widths.label + widths.figures;
    place.figures_x = pieces.left + kClockLineInset + widths.label;
    if (!game_time) {
        if (figures_end > bar_end)
            return ClockLinePlace{};
        place.spot = ClockLineSpot::first_section;
        return place;
    }
    if (figures_end <= second &&
        second + kClockLineInset + widths.time <= std::min(bar_end, second + section)) {
        place.spot = ClockLineSpot::sections;
        place.time_x = second + kClockLineInset;
        return place;
    }
    const int32_t beside = figures_end + kClockLineBesideGap;
    if (figures_end <= bar_end &&
        beside + std::max(widths.time_label, widths.time_value) <= bar_end) {
        place.spot = ClockLineSpot::beside;
        place.time_x = beside;
        return place;
    }
    return ClockLinePlace{};
}

int32_t clock_line_bar_end(
    const TopBarPieces& pieces, const ClockLineWidths& widths, ClockLineSpot spot
) noexcept {
    const int32_t section = pieces.width / kTopBarPieceSections;
    if (section <= 0)
        return 0;
    const int32_t second = pieces.left + section;
    const int32_t figures_end = pieces.left + kClockLineInset + widths.label + widths.figures;
    if (spot == ClockLineSpot::sections) {
        const int32_t time_end = second + kClockLineInset + widths.time;
        return figures_end <= second && time_end <= second + section ? time_end : 0;
    }
    if (spot == ClockLineSpot::beside)
        return figures_end + kClockLineBesideGap + std::max(widths.time_label, widths.time_value);
    if (spot == ClockLineSpot::first_section)
        return figures_end;
    return 0;
}

double clock_line_chrome_scale(
    const TopBarPieces& pieces,
    const ClockLineWidths& widths,
    bool game_time,
    int32_t window_width,
    int32_t bar_end,
    double scale
) noexcept {
    if (window_width <= kClockLineBattlefieldMaxWidth || scale <= 0.0)
        return scale;
    // A bar from the side column's edge to the window's at a scale ends at
    // the window's width in source columns, or a column past it. A bar
    // drawn smaller for the line reaches as far past it as the line starts
    // into its section.
    const auto reaching = [window_width](int32_t end) {
        return static_cast<double>(window_width) / (end + kClockLineInset);
    };
    if (game_time) {
        if (const int32_t sections = clock_line_bar_end(pieces, widths, ClockLineSpot::sections);
            sections > 0) {
            if (bar_end >= sections)
                return scale;
            if (const double smaller = reaching(sections); smaller >= kClockLineSectionsLeastScale)
                return std::min(scale, smaller);
        }
    }
    const int32_t end = clock_line_bar_end(
        pieces, widths, game_time ? ClockLineSpot::beside : ClockLineSpot::first_section
    );
    if (end <= 0 || bar_end >= end)
        return scale;
    return std::min(scale, reaching(end));
}

ClockLineRows clock_line_battlefield_rows(bool game_time) noexcept {
    ClockLineRows rows{};
    int32_t y = kClockLineTop;
    if (game_time) {
        rows.time = y;
        y += kClockLineStep;
    }
    rows.wind = y;
    rows.tidal = y + kClockLineStep;
    return rows;
}

ClockLineParts split_clock_line(std::string_view line) noexcept {
    constexpr std::string_view kLabelEnd = " : ";
    ClockLineParts parts{};
    const auto at = line.find(kLabelEnd);
    if (at == std::string_view::npos) {
        parts.label = line;
        return parts;
    }
    parts.label = line.substr(0, at + kLabelEnd.size());
    auto after = line.substr(parts.label.size());
    if (after.starts_with('+')) {
        const auto end = std::min(after.find(' '), after.size());
        parts.amount = after.substr(0, end);
        after = after.substr(end);
    }
    parts.rest = after;
    return parts;
}

} // namespace oa::ui::hud
