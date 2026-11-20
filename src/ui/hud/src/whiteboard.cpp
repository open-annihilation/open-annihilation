// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/hud/whiteboard.hpp"

#include "oa/ui/hud/shared_views.hpp"

#include <algorithm>
#include <cstring>

namespace oa::ui::hud {
namespace {

/// The pad byte of a record.
constexpr uint8_t kPad = 0;
/// Bytes before a marker's text: type, pad, x, y, colour.
constexpr std::size_t kMarkerHeader = 7;
/// Bytes before an edit's text: type, pad, x, y.
constexpr std::size_t kEditHeader = 6;

void put16(std::vector<uint8_t>& out, int32_t value) {
    const auto word = static_cast<uint16_t>(value);
    out.push_back(static_cast<uint8_t>(word & 0xffu));
    out.push_back(static_cast<uint8_t>(word >> 8));
}

int32_t get16(std::span<const uint8_t> bytes, std::size_t at) noexcept {
    return static_cast<int32_t>(static_cast<uint16_t>(bytes[at] | (bytes[at + 1] << 8)));
}

/// Tells whether a point lies in the square of half side `reach` around
/// another, the far edges left out.
bool within(int32_t x, int32_t y, int32_t cx, int32_t cy, int32_t reach) noexcept {
    return x >= cx - reach && x < cx + reach && y >= cy - reach && y < cy + reach;
}

/// Erases the marks whose anchor (a line's first end, a marker's place) a square reaches.
void erase_square(Whiteboard& board, int32_t x, int32_t y, int32_t reach) {
    std::erase_if(board.lines, [&](const WhiteboardLine& line) {
        return within(line.x1, line.y1, x, y, reach);
    });
    std::erase_if(board.markers, [&](const WhiteboardMarker& marker) {
        return within(marker.x, marker.y, x, y, reach);
    });
}

/// The marker standing exactly at a place, or null.
WhiteboardMarker* marker_placed_at(Whiteboard& board, int32_t x, int32_t y) noexcept {
    for (auto& marker : board.markers)
        if (marker.x == x && marker.y == y)
            return &marker;
    return nullptr;
}

std::vector<uint8_t> line_record(const WhiteboardLine& line) {
    std::vector<uint8_t> record{whiteboard_record::line, kPad};
    put16(record, line.x1);
    put16(record, line.y1);
    record.push_back(line.color);
    record.push_back(kPad);
    put16(record, line.x2);
    put16(record, line.y2);
    return record;
}

std::vector<uint8_t> point_record(uint8_t type, int32_t x, int32_t y) {
    std::vector<uint8_t> record{type, kPad};
    put16(record, x);
    put16(record, y);
    return record;
}

/// Tells whether a record is sent alone: a marker or a text edit.
bool sent_alone(const std::vector<uint8_t>& record) noexcept {
    return !record.empty() &&
           (record[0] == whiteboard_record::marker || record[0] == whiteboard_record::edit_text);
}

/// Reads a NUL-ended text from `at`; false when the batch ends first.
bool read_text(
    std::span<const uint8_t> batch, std::size_t at, std::string& text, std::size_t& end
) {
    for (std::size_t index = at; index < batch.size(); ++index)
        if (batch[index] == 0) {
            text.assign(reinterpret_cast<const char*>(batch.data() + at), index - at);
            end = index + 1;
            return true;
        }
    return false;
}

} // namespace

void whiteboard_draw_line(
    Whiteboard& board, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint8_t color
) {
    const WhiteboardLine line{x1, y1, x2, y2, color};
    board.lines.push_back(line);
    board.outgoing.push_back(line_record(line));
}

void whiteboard_place_marker(
    Whiteboard& board, int32_t x, int32_t y, uint8_t color, const std::string& text
) {
    board.markers.push_back({x, y, color, text});
    std::vector<uint8_t> record{whiteboard_record::marker, kPad};
    put16(record, x);
    put16(record, y);
    record.push_back(color);
    record.insert(record.end(), text.begin(), text.end());
    record.push_back(0);
    board.outgoing.push_back(std::move(record));
}

bool whiteboard_edit_marker(Whiteboard& board, int32_t x, int32_t y, const std::string& text) {
    auto* marker = marker_placed_at(board, x, y);
    if (marker == nullptr)
        return false;
    marker->text = text;
    auto record = point_record(whiteboard_record::edit_text, x, y);
    record.insert(record.end(), text.begin(), text.end());
    record.push_back(0);
    board.outgoing.push_back(std::move(record));
    return true;
}

bool whiteboard_move_marker(Whiteboard& board, int32_t x, int32_t y, int32_t new_x, int32_t new_y) {
    auto* marker = marker_placed_at(board, x, y);
    if (marker == nullptr)
        return false;
    marker->x = new_x;
    marker->y = new_y;
    std::vector<uint8_t> record{whiteboard_record::move, kPad};
    put16(record, x);
    put16(record, y);
    record.push_back(marker->color);
    record.push_back(kPad);
    put16(record, new_x);
    put16(record, new_y);
    board.outgoing.push_back(std::move(record));
    return true;
}

void whiteboard_spot_erase(Whiteboard& board, int32_t x, int32_t y) {
    erase_square(board, x, y, kWhiteboardSpotReach);
    board.outgoing.push_back(point_record(whiteboard_record::spot_erase, x, y));
}

void whiteboard_area_erase(Whiteboard& board, int32_t x, int32_t y) {
    erase_square(board, x, y, kWhiteboardAreaReach);
    board.outgoing.push_back(point_record(whiteboard_record::area_erase, x, y));
}

int32_t whiteboard_marker_at(const Whiteboard& board, int32_t x, int32_t y) noexcept {
    for (std::size_t index = board.markers.size(); index-- > 0;)
        if (within(board.markers[index].x, board.markers[index].y, x, y, kWhiteboardSpotReach))
            return static_cast<int32_t>(index);
    return -1;
}

std::vector<uint8_t> whiteboard_take_batch(Whiteboard& board) {
    std::vector<uint8_t> batch;
    if (board.outgoing.empty())
        return batch;
    batch.push_back(0);
    std::size_t taken = 0;
    if (sent_alone(board.outgoing.front())) {
        batch[0] = 1;
        const auto& record = board.outgoing.front();
        batch.insert(batch.end(), record.begin(), record.end());
        taken = 1;
    } else {
        while (taken < board.outgoing.size() && !sent_alone(board.outgoing[taken]) &&
               batch.size() + kWhiteboardLongRecord < kWhiteboardBatchBytes) {
            const auto& record = board.outgoing[taken];
            batch.insert(batch.end(), record.begin(), record.end());
            ++batch[0];
            ++taken;
        }
    }
    board.outgoing.erase(
        board.outgoing.begin(), board.outgoing.begin() + static_cast<std::ptrdiff_t>(taken)
    );
    return batch;
}

uint32_t whiteboard_apply_batch(
    Whiteboard& board, std::span<const uint8_t> batch, const WhiteboardEcho& echo
) {
    if (batch.empty())
        return 0;
    const uint8_t count = batch[0];
    std::size_t at = 1;
    uint32_t applied = 0;
    for (uint8_t index = 0; index < count && at < batch.size(); ++index) {
        const uint8_t type = batch[at];
        if (type == whiteboard_record::line || type == whiteboard_record::move) {
            if (at + kWhiteboardLongRecord > batch.size())
                break;
            const int32_t x1 = get16(batch, at + 2);
            const int32_t y1 = get16(batch, at + 4);
            const uint8_t color = batch[at + 6];
            const int32_t x2 = get16(batch, at + 8);
            const int32_t y2 = get16(batch, at + 10);
            if (type == whiteboard_record::line) {
                board.lines.push_back({x1, y1, x2, y2, color});
            } else if (auto* marker = marker_placed_at(board, x1, y1)) {
                marker->x = x2;
                marker->y = y2;
            }
            at += kWhiteboardLongRecord;
        } else if (type == whiteboard_record::spot_erase || type == whiteboard_record::area_erase) {
            if (at + kWhiteboardShortRecord > batch.size())
                break;
            erase_square(
                board,
                get16(batch, at + 2),
                get16(batch, at + 4),
                type == whiteboard_record::spot_erase ? kWhiteboardSpotReach : kWhiteboardAreaReach
            );
            at += kWhiteboardShortRecord;
        } else if (type == whiteboard_record::marker) {
            std::string text;
            std::size_t end = 0;
            if (at + kMarkerHeader > batch.size() ||
                !read_text(batch, at + kMarkerHeader, text, end))
                break;
            const int32_t x = get16(batch, at + 2);
            const int32_t y = get16(batch, at + 4);
            const uint8_t color = batch[at + 6];
            board.markers.push_back({x, y, color, text});
            board.received_marker = true;
            board.received_x = x;
            board.received_y = y;
            if (echo.marker != nullptr)
                echo.marker(echo.user, color, text.c_str());
            at = end;
        } else if (type == whiteboard_record::edit_text) {
            std::string text;
            std::size_t end = 0;
            if (at + kEditHeader > batch.size() || !read_text(batch, at + kEditHeader, text, end))
                break;
            if (auto* marker = marker_placed_at(board, get16(batch, at + 2), get16(batch, at + 4)))
                marker->text = text;
            at = end;
        } else {
            break;
        }
        ++applied;
    }
    return applied;
}

std::string whiteboard_echo_line(const World& world, uint8_t color, const std::string& text) {
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        const Player& player = world.game.players[slot];
        if (player.in_use == 0 || stock_dot_color(world, slot) != color)
            continue;
        const std::string name(player.name, ::strnlen(player.name, sizeof player.name));
        return text.empty() ? "*" + name + " added a new marker" : "*" + name + ": " + text;
    }
    return {};
}

} // namespace oa::ui::hud
