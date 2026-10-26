// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// 8-bit drawing over an RGB frame. The model routines draw into
// palette-indexed surfaces and blend through palette tables, so they need the
// indices of what is already on screen. A bridge lays an 8-bit surface over a
// rectangle of an RGB frame (optionally scaled): regions are captured on
// demand by mapping their RGB pixels back to palette indices, drawing is
// clipped to the captured region, and at the end every 8-bit pixel that
// changed is written back through the palette. Unchanged pixels keep their
// RGB values, so colours outside the palette survive untouched.
//
// The bridge keeps the indices it maps from one capture to the next, with
// the colours they were mapped from, so that a capture maps again only the
// frame pixels whose colour has changed since, whoever changed it: the
// bridge's own write-backs, or sprites and particles drawn straight into the
// frame. A capture gives the same indices as mapping every pixel afresh.
//
// This is presentation support for the RGB match frame; it changes no 3.1c
// behaviour.

#include "oa/present/surface.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

namespace oa::present::model {

/// Side of the square tiles, in 8-bit pixels, a bridge captures and writes
/// back.
inline constexpr int32_t bridge_tile_side = 32;

/// The share of physical memory a bridge's picture and captured indices
/// together may take. The same share of the free address space caps a
/// 32-bit build.
inline constexpr uint64_t bridge_memory_share = 8;

/// Bytes a bridge may take when the machine does not say how much physical
/// memory it has: 512 MiB.
inline constexpr uint64_t bridge_unknown_budget = uint64_t{512} << 20;

/// Bytes a 32-bit bridge may take when the free address space is not known:
/// 256 MiB.
inline constexpr uint64_t bridge_address_cap = uint64_t{256} << 20;

/// The most a 32-bit address can name. A larger report of free address
/// space is cut to this before the share is taken, so the budget stays
/// within what a 32-bit process can address.
inline constexpr uint64_t bridge_address_space = (uint64_t{1} << 32) - 1;

/// Begins in a row, with the picture at most half the buffers, after which
/// the buffers are released down to the picture.
inline constexpr uint32_t bridge_release_frames = 45;

/// Returns the bytes a bridge's picture and captured indices together may take.
///
/// An eighth of `physical`, or bridge_unknown_budget when `physical` is 0.
/// On a 32-bit address (`address_is_32_bit`), also at most an eighth of
/// `available_virtual`, cut to bridge_address_space first, or
/// bridge_address_cap when that is unknown. Zero free address space is a
/// real reading and yields no bytes.
///
/// @param physical physical memory, in bytes; 0 when it is not known
/// @param available_virtual free address space, in bytes; nothing when it
///     is not known, and 0 when none is free
/// @param address_is_32_bit true when a pointer holds 32 bits
/// @return the budget, in bytes
[[nodiscard]] inline uint64_t bridge_byte_budget(
    uint64_t physical, std::optional<uint64_t> available_virtual, bool address_is_32_bit
) noexcept {
    const uint64_t budget = physical == 0 ? bridge_unknown_budget : physical / bridge_memory_share;
    if (!address_is_32_bit)
        return budget;
    const uint64_t address =
        available_virtual ? std::min(*available_virtual, bridge_address_space) / bridge_memory_share
                          : bridge_address_cap;
    return address < budget ? address : budget;
}

/// Bytes a bridge holds for its 8-bit picture, or for the indices it captured.
///
/// Taken with malloc, so a failure returns to the caller. The block is not
/// copied; a moved-from block is empty.
struct BridgeBytes {
    uint8_t* storage{};  ///< the block, or null
    std::size_t count{}; ///< bytes in use
    std::size_t room{};  ///< bytes the block holds

    /// Returns the block.
    ///
    /// @return the block, or null
    [[nodiscard]] uint8_t* data() noexcept { return storage; }

    /// Returns the block, read only.
    ///
    /// @return the block, or null
    [[nodiscard]] const uint8_t* data() const noexcept { return storage; }

    /// Returns the bytes in use.
    ///
    /// @return the bytes in use
    [[nodiscard]] std::size_t size() const noexcept { return count; }

    /// Returns the bytes the block holds.
    ///
    /// @return the bytes the block holds
    [[nodiscard]] std::size_t capacity() const noexcept { return room; }

    /// Frees the block and empties it.
    void release() noexcept {
        std::free(storage);
        storage = nullptr;
        count = 0;
        room = 0;
    }

    BridgeBytes() = default;
    BridgeBytes(const BridgeBytes&) = delete;
    BridgeBytes& operator=(const BridgeBytes&) = delete;

    /// Takes another block's bytes, leaving it empty.
    ///
    /// @param[in,out] other the block given up
    BridgeBytes(BridgeBytes&& other) noexcept { *this = std::move(other); }

    /// Frees this block and takes another's bytes, leaving it empty.
    ///
    /// @param[in,out] other the block given up
    /// @return this block
    BridgeBytes& operator=(BridgeBytes&& other) noexcept {
        if (this != &other) {
            release();
            storage = other.storage;
            count = other.count;
            room = other.room;
            other.storage = nullptr;
            other.count = 0;
            other.room = 0;
        }
        return *this;
    }

    /// Frees the block.
    ~BridgeBytes() { release(); }
};

/// A tile state bit: the bridge's capture copy holds the colour and index of
/// every frame pixel the tile captures from, as last mapped.
inline constexpr uint8_t bridge_tile_mapped = 1;

/// A tile state bit: the tile was captured since the bridge was last written
/// back, so its 8-bit pixels may hold draws.
inline constexpr uint8_t bridge_tile_open = 2;

// A packed 24-bit RGB frame.
struct RgbFrame {
    uint8_t* rgb{};
    int32_t width{};
    int32_t height{};
    int32_t stride{}; // bytes per row
};

/// The frame pixels a bridge captures from, each with its colour when it
/// was last mapped to a palette index and that index, kept from one capture
/// to the next. Each 8-bit pixel captures from one frame pixel; pixels that
/// capture from the same frame row (or column) share a row (or column) of
/// the copy, so the copy never holds more pixels than the frame.
struct CaptureCopy {
    std::vector<uint8_t> colours; ///< red, green and blue of each pixel, row by row
    std::vector<uint8_t> indices; ///< the palette index of each pixel's colour, row by row
    int32_t width{};              ///< pixels in a row of the copy
    int32_t height{};             ///< rows of the copy
    /// The copy's column each 8-bit column captures from.
    std::vector<int32_t> column_of;
    /// The copy's row each 8-bit row captures from.
    std::vector<int32_t> row_of;
    /// Byte offset, within a frame row, of each column of the copy.
    std::vector<std::ptrdiff_t> frame_columns;
    /// Byte offset, within the frame, of the start of each row of the copy.
    std::vector<std::ptrdiff_t> frame_rows;
    /// True when every 8-bit column has a column of the copy of its own and
    /// they lie side by side in the frame, as at a scale of 1, so that a
    /// row of a tile compares and copies as one run.
    bool columns_side_by_side{};
    /// True when the copy's columns lie side by side in the frame, as when
    /// the bridge is drawn smaller than its 8-bit view, so that the copy's
    /// part of a row of a tile compares as one run even where several
    /// 8-bit columns share a column of the copy.
    bool copy_columns_side_by_side{};
    /// True when each 8-bit pixel captures from the one frame pixel it is
    /// written back to, as at a scale of 1 over a rectangle inside the
    /// frame, so that bridge_end brings the copy up to date with the pixels
    /// it writes.
    bool written_in_place{};
    /// The palette index of each palette entry's colour: the entry itself,
    /// or the first entry of the same colour.
    std::array<uint8_t, OA_PALETTE_COLORS> entry_indices{};
    /// True once the rows and columns above are laid out for the bridge's
    /// surface, area and frame; bridge_begin clears it when they change.
    bool laid_out{};
    /// Frame pixels mapped to a palette index by captures since the bridge
    /// was made: the measure of the work the copy saves.
    uint64_t mapped_pixels{};
};

struct RgbBridge {
    Surface surface{}; // the 8-bit view the routines draw into
    BridgeBytes pixels;
    BridgeBytes baseline; // indices as captured
    /// Each tile's state, row by row: bridge_tile_mapped and bridge_tile_open.
    std::vector<uint8_t> tiles;
    /// The tiles captured since the bridge was last written back, by their
    /// place in `tiles`.
    std::vector<uint32_t> open_tiles;
    int32_t tiles_x{};
    int32_t tiles_y{};
    CaptureCopy copy{};
    RgbFrame frame{};
    Rect32 area{};     // RGB rectangle covered, inclusive
    float scale{1.0F}; // RGB pixels per 8-bit pixel
    /// How far into its 8-bit column the rectangle's first RGB column
    /// starts, in 16.16 parts of the scale's step (oa/present/scene_grid.hpp's
    /// phase), as the terrain under it was filled.
    uint32_t phase_x{};
    uint32_t phase_y{}; ///< the same for the rectangle's first RGB row
    Palette palette{};
    std::vector<uint32_t> exact_keys; // open-addressed RGB -> index for palette colours
    std::vector<uint8_t> exact_values;
    /// Other colours and the index of their nearest entry, one slot per
    /// colour's hash: a colour remembered there is not searched for again
    /// until another colour takes its slot.
    std::vector<uint32_t> nearest_keys;
    std::vector<uint8_t> nearest_values; ///< by slot, as nearest_keys
    /// Counts the times bridge_begin built the colour lookup afresh (a new
    /// palette): a band's own colour memory from before is forgotten.
    uint32_t lookup_builds{};
    /// Bytes the picture and the captured indices together may take. Zero
    /// means no limit. A begin that would pass it keeps the buffers it has
    /// and returns false.
    uint64_t byte_budget{};
    /// Bytes the last refused begin asked for, picture and indices together.
    uint64_t refused_bytes{};
    /// Times the picture's buffers grew.
    uint32_t buffer_growths{};
    /// Begins in a row whose picture needed at most half the buffers.
    uint32_t small_frames{};
};

/// Covers a rectangle of an RGB frame with an 8-bit surface of the rectangle's size divided by the scale.
///
/// Each 8-bit pixel is written back to the frame pixels the scene grid
/// lays its place over at the scale (oa/present/scene_grid.hpp), as the
/// terrain fill lays the map pixel of the same place, so that a model drawn
/// on a map pixel lands over that pixel's ground. Nothing is captured yet.
/// The colour lookup is rebuilt when the palette changes. The capture copy
/// is kept when the frame's size and row length, the rectangle, the scale,
/// the phases and the palette are those of the last call, so
/// that captures map again only the colours that changed, whatever the frame
/// holds now; otherwise it is forgotten.
///
/// The picture and the captured indices grow with headroom, at most to the
/// bridge's byte budget (RgbBridge::byte_budget; zero means no limit), and
/// are released down to the picture after bridge_release_frames begins that
/// needed at most half of them. They are taken with malloc. A picture that
/// would pass the budget, or that cannot be allocated, or whose size does
/// not fit the surface, leaves the bridge unusable for that picture and
/// returns false; the process carries on. The bytes asked for are
/// RgbBridge::refused_bytes.
///
/// @param[in,out] bridge bridge to set up; its buffers are reused
/// @param frame RGB frame; must outlive the bridge's use
/// @param area inclusive RGB rectangle to cover
/// @param scale RGB pixels per 8-bit pixel; non-positive counts as 1
/// @param palette palette the indices refer to
/// @param phase_x how far into the 8-bit column 0 the rectangle's first
///     RGB column starts, 16.16 parts of the scale's step, below it
/// @param phase_y the same for the 8-bit row 0 and the first RGB row
/// @return false when the picture cannot be held
bool bridge_begin(
    RgbBridge& bridge,
    const RgbFrame& frame,
    const Rect32& area,
    float scale,
    const Palette& palette,
    uint32_t phase_x = 0,
    uint32_t phase_y = 0
);

/// Makes the bridge's picture allocation fail while set, so a begin that
/// needs a new block returns false. The process's new handler is not called.
///
/// @param fail true to refuse the next picture allocations
void bridge_fail_allocations(bool fail) noexcept;

/// Captures the tiles overlapping a region and clips the surface to it.
///
/// Each tile not yet captured since the last bridge_end starts its 8-bit
/// pixels as the palette indices of the frame pixels they capture from,
/// which the capture copy gives, mapping again only the frame pixels whose
/// colour changed since the copy last mapped them. Draws can then only
/// change captured pixels; a region outside the surface leaves an empty
/// clip that rejects every draw.
///
/// @param[in,out] bridge bridge set up by bridge_begin
/// @param region inclusive rectangle in 8-bit pixels
void bridge_open(RgbBridge& bridge, const Rect32& region);

/// Writes every changed pixel of the captured tiles back to the frame through the palette.
///
/// Every tile counts as uncaptured afterwards. The capture copy keeps what it
/// holds: the next capture finds the pixels written back by their changed
/// colours.
///
/// @param[in,out] bridge bridge whose captured tiles are committed
void bridge_end(RgbBridge& bridge);

/// Slots of the colours outside the palette a bridge remembers the nearest
/// entry of; a power of two.
inline constexpr std::size_t bridge_nearest_slots = std::size_t{1} << 16;

/// An 8-bit surface laid over a region of a bridge's surface at a whole
/// number of samples along each axis of a bridge pixel, for drawing finer
/// than the bridge (enhanced anti-aliasing). Writing it back blends each
/// frame pixel with the samples drawn over its bridge pixel.
struct SampledRegion {
    Surface surface{};             ///< the samples the routines draw into
    std::vector<uint8_t> samples;  ///< the surface's pixels
    std::vector<uint8_t> baseline; ///< each bridge pixel's index as captured, row by row
    /// The first sample row of each bridge row as captured, so that a sample
    /// row nothing was drawn into is passed over at once.
    std::vector<uint8_t> captured_rows;
    Rect32 region{0, 0, -1, -1}; ///< bridge pixels covered, inclusive; empty when x1 > x2
    uint32_t factor{1};          ///< samples along each axis of a bridge pixel
};

/// Covers a region of a bridge's surface with samples.
///
/// The region is clipped to the bridge's surface. Each bridge pixel is
/// captured as bridge_open captures it, and every sample of it starts as
/// that index, so that blended draws blend with what lies under them. The
/// surface's clip is the whole region. The bridge's tiles are not captured;
/// the capture copy of those not captured already is brought up to date
/// with the frame over them, and their 8-bit pixels start again from it.
///
/// @param bridge bridge set up by bridge_begin; its colour lookup learns
///     the colours captured, and its capture copy the frame under the region
/// @param[out] sampled region to set up; its buffers are reused
/// @param region inclusive rectangle in the bridge's 8-bit pixels
/// @param factor samples along each axis of a bridge pixel; 0 counts as 1
void bridge_open_sampled(
    RgbBridge& bridge, SampledRegion& sampled, const Rect32& region, uint32_t factor
);

/// Writes a sampled region back to the frame.
///
/// A sample whose index differs from its bridge pixel's captured index was
/// drawn. Each frame pixel of a bridge pixel with drawn samples becomes the
/// average of its samples, a drawn one counting as its palette colour and
/// one not drawn as the frame pixel's own colour, rounded to nearest. A
/// bridge pixel drawn all over in one index becomes that palette colour, as
/// bridge_end writes it, and one with no sample drawn is left as it is.
/// Frame pixels map to bridge pixels as bridge_end maps them.
///
/// @param bridge bridge whose frame receives the samples
/// @param sampled region set up by bridge_open_sampled and drawn into
void bridge_end_sampled(const RgbBridge& bridge, const SampledRegion& sampled);

/// Returns the palette index of an RGB colour.
///
/// @param[in,out] bridge bridge whose lookup caches nearest matches
/// @param r red
/// @param g green
/// @param b blue
/// @return the index of an exactly matching entry, else of the nearest by squared distance
[[nodiscard]] uint8_t bridge_index(RgbBridge& bridge, uint8_t r, uint8_t g, uint8_t b);

/// A band of a bridge: whole rows of its tiles, and the frame rows they
/// capture from and are written back to. A frame drawn band by band, each
/// band drawing every draw of the frame in order through the band
/// functions below, changes only the band's rows, of the 8-bit surface and
/// of the frame, and reads only them: it comes out byte for byte as the
/// frame drawn whole, and bands could draw at the same time.
///
/// What a band keeps of its own: its surface's clip, the tiles it captured,
/// and the colours outside the palette it remembers the nearest entry of
/// when it has room for them (bridge_band_colours). The 8-bit pixels, the
/// tiles' states and the capture copy are the bridge's, each row of them
/// one band's alone.
struct BridgeBand {
    /// The bridge's 8-bit pixels, its draws limited to the band's rows
    /// (surface_band); bridge_open sets its clip.
    Surface surface{};
    int32_t first_row{};       ///< the band's first 8-bit row: whole tiles down
    int32_t end_row{};         ///< the 8-bit row after its last
    int32_t frame_first_row{}; ///< the first frame row the band captures from and writes
    int32_t frame_end_row{};   ///< the frame row after the last
    /// The tiles captured since the band was last written back, by their
    /// place in RgbBridge::tiles.
    std::vector<uint32_t> open_tiles;
    /// Colours outside the palette and the index of their nearest entry, as
    /// RgbBridge::nearest_keys and nearest_values hold them; empty while the
    /// band shares the bridge's, which only one band drawing at a time may.
    std::vector<uint32_t> nearest_keys;
    std::vector<uint8_t> nearest_values; ///< by slot, as nearest_keys
    /// The bridge's RgbBridge::lookup_builds when the band's own colour
    /// memory was last emptied: the memory is emptied again when they differ.
    uint32_t lookup_builds{};
    /// Frame pixels the band's captures mapped, to be added to the bridge's
    /// count (CaptureCopy::mapped_pixels) by bridge_join_band.
    uint64_t mapped_pixels{};
};

/// Splits a bridge set up by bridge_begin into bands of whole tile rows.
///
/// The bands are about even in frame rows. A band's edge lies only between
/// two tile rows where the frame rows split as well: every 8-bit row above
/// it captures from and is written back to frame rows above the first frame
/// row of the band below, and every row below it from and to that row or
/// further down. At a scale of 1 every tile row can be an edge; at another
/// scale some cannot, and the bands are fewer when too few can. The first
/// band's frame rows start at the frame's top and the last's end at its
/// bottom, so that the bands' frame rows cover the frame. The capture copy
/// is laid out for the bridge, so that no band lays it out while it draws.
///
/// @param[in,out] bridge bridge set up by bridge_begin, no tile captured
/// @param count bands wanted; less than 1 counts as 1
/// @param[out] bands resized to the bands made, each with nothing captured;
///     a band keeps the room bridge_band_colours gave it, emptied when the
///     bridge's colour lookup was built afresh since
/// @return the bands made: at least 1, at most `count` and the tile rows
int32_t bridge_split(RgbBridge& bridge, int32_t count, std::vector<BridgeBand>& bands);

/// Gives a band room of its own for the colours outside the palette, so that
/// it never writes the bridge's while another band draws.
///
/// @param bridge the bridge the band belongs to
/// @param[in,out] band band whose nearest_keys and nearest_values get
///     bridge_nearest_slots empty slots; kept when it has them and the
///     bridge's colour lookup was not built afresh since they were emptied
void bridge_band_colours(const RgbBridge& bridge, BridgeBand& band);

/// Captures the tiles of a band overlapping a region, as bridge_open does.
///
/// The band's surface gets the clip bridge_open would give the bridge's,
/// whatever the band: its draws work out what they draw as over the whole
/// bridge and change only the band's rows.
///
/// @param[in,out] bridge bridge split by bridge_split
/// @param[in,out] band one of its bands
/// @param region inclusive rectangle in 8-bit pixels
void bridge_open(RgbBridge& bridge, BridgeBand& band, const Rect32& region);

/// Writes a band's captured tiles back to the frame, as bridge_end does.
///
/// @param[in,out] bridge bridge split by bridge_split
/// @param[in,out] band one of its bands
void bridge_end(RgbBridge& bridge, BridgeBand& band);

/// Covers a region of a bridge's surface with samples for a band's rows, as
/// bridge_open_sampled covers it whole.
///
/// The samples are laid out for the whole region, so that a model draws
/// into them as into the whole; only the bridge rows of the band are
/// captured, and the samples' draws change only the sample rows over them
/// (surface_band).
///
/// @param bridge bridge split by bridge_split; its tiles and capture copy
///     in the band's rows are brought up to date
/// @param[in,out] band one of its bands, whose colour lookup learns the
///     colours captured
/// @param[out] sampled region to set up; its buffers are reused
/// @param region inclusive rectangle in the bridge's 8-bit pixels
/// @param factor samples along each axis of a bridge pixel; 0 counts as 1
void bridge_open_sampled(
    RgbBridge& bridge,
    BridgeBand& band,
    SampledRegion& sampled,
    const Rect32& region,
    uint32_t factor
);

/// Writes the band's rows of a sampled region back to the frame, as
/// bridge_end_sampled writes them.
///
/// @param bridge bridge whose frame receives the samples
/// @param band the band the region was set up for
/// @param sampled region set up by the band's bridge_open_sampled and drawn into
void bridge_end_sampled(
    const RgbBridge& bridge, const BridgeBand& band, const SampledRegion& sampled
);

/// Returns the palette index of an RGB colour, as bridge_index does, through
/// the band's own colour memory when it has one.
///
/// @param[in,out] bridge bridge whose lookup caches nearest matches unless
///     the band has room of its own
/// @param[in,out] band band whose lookup caches them when it has room
/// @param r red
/// @param g green
/// @param b blue
/// @return the index of an exactly matching entry, else of the nearest by squared distance
[[nodiscard]] uint8_t
bridge_index(RgbBridge& bridge, BridgeBand& band, uint8_t r, uint8_t g, uint8_t b);

/// Adds the frame pixels a band's captures mapped to the bridge's count.
///
/// @param[in,out] bridge bridge whose CaptureCopy::mapped_pixels grows
/// @param[in,out] band band whose count goes back to 0
void bridge_join_band(RgbBridge& bridge, BridgeBand& band) noexcept;

} // namespace oa::present::model
