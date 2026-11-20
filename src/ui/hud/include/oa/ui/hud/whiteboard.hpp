// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The allied whiteboard of ui.whiteboard: lines, dots and text markers a
// player draws on the map for their allies, kept in map pixels, the records
// each edit sends to the other machines and the batches they arrive in.
// Network play carries the batches; this module makes and applies them.
#pragma once

#include "oa/core/world.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace oa::ui::hud {

/// Record types of a whiteboard batch.
namespace whiteboard_record {
inline constexpr uint8_t line = 1;        ///< x1, y1, colour, pad, x2, y2
inline constexpr uint8_t marker = 3;      ///< x, y, colour, text: a dot when the text is empty
inline constexpr uint8_t edit_text = 5;   ///< x, y of the marker, its new text
inline constexpr uint8_t move = 6;        ///< old x, y, colour, pad, new x, y
inline constexpr uint8_t spot_erase = 10; ///< x, y
inline constexpr uint8_t area_erase = 11; ///< x, y
} // namespace whiteboard_record

/// Half the side of the square a spot erase, and a press on a marker, reach.
inline constexpr int32_t kWhiteboardSpotReach = 10;
/// Half the side of the square an area erase reaches around each point of its path.
inline constexpr int32_t kWhiteboardAreaReach = 50;
/// A batch holds no more bytes than this, its count byte included.
inline constexpr std::size_t kWhiteboardBatchBytes = 99;
/// Bytes of a line or move record.
inline constexpr std::size_t kWhiteboardLongRecord = 12;
/// Bytes of an erase record.
inline constexpr std::size_t kWhiteboardShortRecord = 6;
/// Side of a dot marker in pixels.
inline constexpr int32_t kWhiteboardDotSide = 10;

struct WhiteboardLine {
    int32_t x1{}, y1{}, x2{}, y2{}; ///< ends in map pixels
    uint8_t color{};                ///< the drawing player's dot colour
};

struct WhiteboardMarker {
    int32_t x{}, y{}; ///< place in map pixels
    uint8_t color{};
    std::string text; ///< empty for a dot
};

/// The whiteboard's marks and the records waiting to be sent.
struct Whiteboard {
    std::vector<WhiteboardLine> lines;
    std::vector<WhiteboardMarker> markers;
    /// Records made here, each whole, oldest first.
    std::vector<std::vector<uint8_t>> outgoing;
    /// The newest marker another machine sent, for Ctrl and the whiteboard key.
    bool received_marker{};
    int32_t received_x{};
    int32_t received_y{};
};

/// What the pointer is doing on the whiteboard, kept between events.
struct WhiteboardInput {
    bool drawing{};   ///< the left button draws a line from (last_x, last_y)
    bool erasing{};   ///< the right button wipes along its path
    int32_t last_x{}; ///< the stroke's last point, in map pixels
    int32_t last_y{};
    int32_t moving{-1}; ///< the marker the left button carries, -1 for none
    int32_t from_x{};   ///< where the carried marker stood
    int32_t from_y{};
    bool editing{};        ///< the text editor is open
    bool editing_marker{}; ///< it edits the marker at (edit_x, edit_y), else places one there
    int32_t edit_x{};
    int32_t edit_y{};
    std::string edit_text;
};

/// Draws a line and queues its record.
void whiteboard_draw_line(
    Whiteboard& board, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint8_t color
);

/// Places a marker (a dot for empty text) and queues its record.
void whiteboard_place_marker(
    Whiteboard& board, int32_t x, int32_t y, uint8_t color, const std::string& text
);

/// Gives the marker at a place new text and queues the edit.
///
/// @return false when no marker stands there
bool whiteboard_edit_marker(Whiteboard& board, int32_t x, int32_t y, const std::string& text);

/// Moves the marker at a place and queues the move.
///
/// @return false when no marker stands there
bool whiteboard_move_marker(Whiteboard& board, int32_t x, int32_t y, int32_t new_x, int32_t new_y);

/// Erases the marks a spot erase at a place reaches and queues it.
void whiteboard_spot_erase(Whiteboard& board, int32_t x, int32_t y);

/// Erases the marks an area erase at a place reaches and queues it.
void whiteboard_area_erase(Whiteboard& board, int32_t x, int32_t y);

/// Returns the marker a press at a place lands on: the last placed whose
/// place is within kWhiteboardSpotReach (the square's far edges left out).
///
/// @return its index, or -1
[[nodiscard]] int32_t whiteboard_marker_at(const Whiteboard& board, int32_t x, int32_t y) noexcept;

/// Takes the next batch to send: a text record alone, or as many other
/// records as fit with 12 bytes to spare under kWhiteboardBatchBytes. The
/// batch starts with its record count.
///
/// @param[in,out] board records waiting; those taken are dropped
/// @return the batch; empty when nothing waits
[[nodiscard]] std::vector<uint8_t> whiteboard_take_batch(Whiteboard& board);

/// What a received batch reports to the player.
struct WhiteboardEcho {
    void* user{};
    /// A received marker: its colour and text (empty for a dot).
    void (*marker)(void* user, uint8_t color, const char* text){};
};

/// Applies a batch another machine sent. A marker record also becomes the
/// newest received marker and is echoed. A batch cut short applies the
/// records it holds whole.
///
/// @param[in,out] board the marks
/// @param batch the batch, its count byte first
/// @param echo where received markers are reported
/// @return the records applied
uint32_t whiteboard_apply_batch(
    Whiteboard& board, std::span<const uint8_t> batch, const WhiteboardEcho& echo
);

/// Formats a received marker's chat line: "*<player>: <text>", or
/// "*<player> added a new marker" for a dot, the player found by the game's
/// own dot index; no line when no player has the colour.
///
/// @param world players
/// @param color the marker's colour
/// @param text its text
/// @return the line, or empty
[[nodiscard]] std::string
whiteboard_echo_line(const World& world, uint8_t color, const std::string& text);

} // namespace oa::ui::hud
