// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Card-ready pages of GAF sprite frames for the world the graphics card
// draws. Each frame is decoded once into RGBA8 texels through the palette
// and the display gamma, packed into a power-of-two page in a cell aligned
// so that a half-size level of the page is exact, found again by the
// caller's frame number and the mode it is drawn in, and evicted least
// recently used first under a named memory limit. The frames a frame drawn
// on the battlefield uses are held until the next one begins, and where they
// alone fill the limit, the limit grows, as far as the caller allows. Pure
// C++20: nothing here touches SDL or the card.
//
// The packing contract. A frame's cell is the frame with frame_gutter
// transparent texels on each side, its width and height rounded up to a
// multiple of cell_alignment, at a corner that is a multiple of it; the
// frame's rectangle begins frame_gutter texels inside the cell's corner.
// So a level 1 of a page computed with the exact 2x2 box holds each frame's
// texels within its own cell with a one-texel gutter around them. The pages
// keep level 0 only for now; the cells are aligned for level 1 to follow.

#include "oa/formats/gaf.hpp"
#include "oa/present/gpu_world/texel.hpp"
#include "oa/present/surface.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace oa::present::gpu_world {

/// Transparent texels left around every frame on a page, on each side: two,
/// so that a half-size level of the page keeps one.
inline constexpr uint32_t frame_gutter = 2;
/// Multiple every cell's corner and sides keep to: two, so that each 2x2 box
/// of a half-size level lies within one cell.
inline constexpr uint32_t cell_alignment = 2;
/// Smallest page side the limits accept.
inline constexpr uint32_t min_page_size = 64;
/// Largest page side: a texture size every card the engine draws on holds.
inline constexpr uint32_t max_page_size = 2048;
/// Page side the limits default to: 4 MiB of texels a page.
inline constexpr uint32_t default_page_size = 1024;
/// Texel memory the limits default to: eight default pages, or two of the
/// largest.
inline constexpr size_t default_memory_limit = size_t{32} * 1024 * 1024;
/// Alpha of a covered texel: every mode draws a covered pixel opaque.
inline constexpr uint8_t opaque_alpha = 255;
/// Entries of the gray table: one for each palette index.
inline constexpr size_t gray_table_entries = OA_PALETTE_COLORS;

/// How a frame is drawn on the battlefield, which decides its texels.
/// Translucency is not a mode: a translucent feature or shadow is the
/// opaque frame drawn with a vertex alpha of one half.
enum class DrawMode : uint8_t {
    /// Today's keyed copy: a covered pixel replaces what is beneath it.
    opaque,
    /// The frame as the fog grays it: each covered pixel through the gray
    /// table, then opaque.
    greyed,
    /// An explosion's flash, which lights what is under it rather than
    /// covering it: each covered pixel of the light ramp, row r of the
    /// light table (its index less oa::present::shade_ramp_base, below
    /// oa::present::ramp_table_rows), holds r / light_rows_per_doubling of
    /// white in each colour, held to white, and no alpha; every other pixel
    /// holds nothing. Drawn by the card's lighten blend, it makes what is
    /// under it 1 + r / light_rows_per_doubling as bright, as the light
    /// table's row r does, without snapping to the palette. The palette
    /// and gamma do not change its texels.
    lit,
};

/// Number of draw modes; each has its own record of a frame.
inline constexpr size_t draw_mode_count = 3;

/// Rows of the light table over which it lights a colour to twice its
/// brightness: row r scales it by 1 + r / 30.
inline constexpr uint32_t light_rows_per_doubling = 30;

/// Whether a frame was placed, and why not.
enum class FrameStatus : uint8_t {
    ok,
    /// set_palette has not been called.
    no_palette,
    /// A greyed frame was asked for before set_gray_table.
    no_gray_table,
    /// find: the frame is not on a page in that mode.
    not_held,
    /// A width or height of 0: nothing to draw.
    empty,
    /// The pixel or coverage count differs from width * height.
    malformed,
    /// The frame's cell exceeds the largest page.
    too_large,
    /// The memory limit holds no page for the frame, even with every other
    /// frame evicted.
    no_room,
};

/// A rectangle of texels on a page.
struct TexelRect {
    uint16_t x{};
    uint16_t y{};
    uint16_t width{};
    uint16_t height{};

    /// Reports whether the rectangle holds no texel.
    ///
    /// @return true when the width or the height is 0
    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
};

/// Where a frame's texels are and how the card draws them.
struct FrameRecord {
    uint32_t page{};    ///< index into pages()
    TexelRect rect{};   ///< the frame's texels, frame_gutter inside its cell's corner
    int16_t origin_x{}; ///< hotspot in frame texels, as the GAF frame carries it
    int16_t origin_y{};
    DrawMode mode{};
};

/// The answer to a frame request: a record when the status is ok.
struct FrameResult {
    FrameStatus status{};
    FrameRecord record{};
};

/// One page of texels, laid out as the card's texture holds it.
struct Page {
    /// Side in texels; 0 for a released page, whose index a later page may
    /// take again.
    uint32_t size{};
    /// size * size texels of texel_bytes, rows top to bottom, no padding.
    /// Red, green and blue are premultiplied by the alpha, and a
    /// transparent texel is zero in every byte.
    std::vector<uint8_t> texels;
    /// Grows every time the page is made, written or released.
    uint64_t revision{};
    /// The texels written since clear_dirty; empty when none were.
    TexelRect dirty{};
};

/// The sizes the pages keep to.
struct Limits {
    /// Side of an ordinary page, a power of two from min_page_size to
    /// max_page_size.
    uint32_t page_size = default_page_size;
    /// Side of the largest page a frame too big for an ordinary page may
    /// have of its own, at most max_page_size.
    uint32_t largest_page_size = max_page_size;
    /// Bytes of page texels alive at once.
    size_t memory_limit = default_memory_limit;
    /// The most the memory limit grows to where the frames of the frame
    /// under way (SpritePages::begin_frame) fill it and the growth hooks
    /// allow it; at or below memory_limit it never grows.
    size_t largest_memory_limit = default_memory_limit;
};

/// What the pages ask before their memory limit grows: the seam to the
/// memory the machine has.
struct GrowthHooks {
    void* context{};
    /// Says whether the pages may take `bytes` more of page texels than
    /// their limit allows now; null lets the limit never grow.
    bool (*allow)(void* context, size_t bytes){};
};

/// What the pages hold, in bytes and counts.
struct MemoryUse {
    size_t page_bytes{};  ///< texel bytes of the pages alive, released pages excluded
    size_t frame_bytes{}; ///< texel bytes of the frames alive, each its whole cell
    size_t memory_limit{};
    uint32_t pages{};  ///< pages alive
    uint32_t frames{}; ///< frames alive, counting each mode of a frame once
};

/// Running counts since the pages were made.
struct Statistics {
    uint64_t hits{};      ///< requests answered from a page
    uint64_t decodes{};   ///< frames decoded and placed
    uint64_t evictions{}; ///< frames evicted to make room
    uint64_t refusals{};  ///< requests refused
    uint64_t growths{};   ///< times the memory limit grew
    /// Requests refused no_room because the frames of the frame under way
    /// filled the limit and it could grow no further.
    uint64_t held_refusals{};
};

/// Sprite frames decoded once into pages of texels, found by frame number
/// and draw mode, and evicted least recently used first.
class SpritePages {
  public:

    /// Makes empty pages that keep to the limits.
    ///
    /// Page sides outside min_page_size..max_page_size are brought inside
    /// it and rounded up to a power of two; a largest page smaller than the
    /// ordinary page becomes the ordinary page, and a largest memory limit
    /// below the memory limit becomes the memory limit.
    ///
    /// @param limits page sides and the memory limits
    explicit SpritePages(const Limits& limits = Limits{});

    /// Sets what the pages ask before their memory limit grows.
    ///
    /// @param hooks the hooks; empty ones let the limit never grow
    void set_growth_hooks(const GrowthHooks& hooks) noexcept { growth_ = hooks; }

    /// Begins a frame drawn on the battlefield: the frames placed or found
    /// from now until the next begin_frame are held, never evicted to make
    /// room, so that a cell the frame draws from keeps its frame until the
    /// frame has run. Where the held frames alone fill the limit, it grows
    /// as far as the growth hooks allow and largest_memory_limit, doubling
    /// each time; past that, a new frame is refused no_room. Before the
    /// first begin_frame nothing is held.
    void begin_frame() noexcept { ++frame_serial_; }

    /// Brings the memory limit back to the one the pages were made with.
    /// Pages already alive over it stay, and further pages wait until they
    /// fit.
    void reset_memory_limit() noexcept { limits_.memory_limit = made_memory_limit_; }

    /// Sets the palette and display gamma every texel is decoded through.
    ///
    /// Each channel becomes the truncated channel * gamma, clamped at 255,
    /// as the display's device palette has it. A palette or gamma that
    /// differs from the current one empties the pages and starts a new
    /// palette generation; the same values again change nothing.
    ///
    /// @param palette game palette of the match
    /// @param gamma channel multiplier
    void set_palette(const Palette& palette, float gamma);

    /// Sets the gray table greyed frames are decoded through.
    ///
    /// A table that differs from the current one drops the greyed frames
    /// already on a page, since their texels would change, and leaves the
    /// opaque ones; the same table again changes nothing.
    ///
    /// @param table the palette index each index grays to
    void set_gray_table(std::span<const uint8_t, gray_table_entries> table);

    /// Returns the frame in a mode, decoding and placing it when no page
    /// holds it.
    ///
    /// A held frame is answered from its page and counts as used now. A
    /// new one is decoded through the palette, covered pixels opaque and
    /// the rest transparent, and placed in an aligned cell behind its
    /// gutter on a page that has room, else on a new page within the memory
    /// limit, else on the room the least recently used frames leave as they
    /// are evicted, those of the frame under way excepted, else on a new
    /// page of a grown limit (begin_frame).
    ///
    /// @param frame_id the caller's number for the frame, the same for the
    ///     same frame every time; a frame whose pixels change needs forget()
    ///     first
    /// @param mode how the frame is drawn
    /// @param source the frame as the GAF reader renders it
    /// @return the record, or why there is none
    FrameResult frame(uint64_t frame_id, DrawMode mode, const formats::gaf::RenderedFrame& source);

    /// Returns a held frame in a mode, counting it as used now.
    ///
    /// @param frame_id the caller's number for the frame
    /// @param mode how the frame is drawn
    /// @return the record, or not_held
    FrameResult find(uint64_t frame_id, DrawMode mode);

    /// Reports whether a page holds the frame in a mode, without counting a use.
    ///
    /// @param frame_id the caller's number for the frame
    /// @param mode how the frame is drawn
    /// @return true when find would answer ok
    [[nodiscard]] bool holds(uint64_t frame_id, DrawMode mode) const noexcept;

    /// Drops every mode of a frame from the pages.
    ///
    /// @param frame_id the caller's number for the frame; one no page holds
    ///     changes nothing
    void forget(uint64_t frame_id);

    /// Drops every frame and releases every page.
    void clear();

    /// Returns the pages, released ones among them with a size of 0.
    ///
    /// @return the pages, indexed as FrameRecord::page indexes them
    [[nodiscard]] std::span<const Page> pages() const noexcept;

    /// Empties a page's dirty rectangle once its texels have been uploaded.
    ///
    /// @param page index into pages(); one outside them changes nothing
    void clear_dirty(uint32_t page) noexcept;

    /// Returns the limits as the pages keep them.
    ///
    /// @return the limits after normalisation
    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }

    /// Returns what the pages hold.
    ///
    /// @return the bytes and counts
    [[nodiscard]] MemoryUse memory() const noexcept;

    /// Returns the running counts.
    ///
    /// @return the counts since the pages were made
    [[nodiscard]] const Statistics& statistics() const noexcept { return statistics_; }

    /// Returns the number of the palette generation.
    ///
    /// @return a count that grows with every palette or gamma that differs
    ///     from the one before; 0 before the first set_palette
    [[nodiscard]] uint64_t palette_generation() const noexcept { return palette_generation_; }

    /// Reports whether a palette has been set.
    ///
    /// @return true after the first set_palette
    [[nodiscard]] bool has_palette() const noexcept { return has_palette_; }

    /// Reports whether a gray table has been set.
    ///
    /// @return true after the first set_gray_table
    [[nodiscard]] bool has_gray_table() const noexcept { return has_gray_table_; }

  private:

    /// Index of no entry, slot or page.
    static constexpr uint32_t none = UINT32_MAX;
    /// Shelf rows a reused shelf may waste over a frame's height before a
    /// new shelf is opened instead: half the frame's height.
    static constexpr uint32_t shelf_waste_divisor = 2;

    /// One texel's four bytes.
    using Texel = std::array<uint8_t, texel_bytes>;
    /// The texel of every palette index in one draw mode.
    using ModeColours = std::array<Texel, OA_PALETTE_COLORS>;

    /// A frame on a page, with its place in the least-recently-used order.
    struct Entry {
        FrameRecord record{};
        uint64_t frame_id{};
        TexelRect slot{};   ///< the frame's cell: the frame with its gutter, aligned
        uint32_t newer{};   ///< the entry used after this one, or none
        uint32_t older{};   ///< the entry used before this one, or none
        uint64_t used_in{}; ///< the frame serial it was last placed or found in
        bool live{};
    };

    /// A run of texels along a shelf: a frame's slot, or free space.
    struct Slot {
        uint16_t x{};
        uint16_t width{};
        uint32_t entry{}; ///< none for free space
    };

    /// A row of slots of one height.
    struct Shelf {
        uint16_t y{};
        uint16_t height{};
        uint16_t open_x{}; ///< first texel after the last slot
        std::vector<Slot> slots;
    };

    /// The shelves of one page.
    struct PageLayout {
        uint16_t next_shelf_y{}; ///< first row below the last shelf
        std::vector<Shelf> shelves;
    };

    /// Where a place was found or made for a slot.
    struct Placement {
        uint32_t page{};
        uint16_t x{};
        uint16_t y{};
    };

    /// Index of a draw mode in the per-mode tables.
    ///
    /// @param mode draw mode
    /// @return 0 or 1
    [[nodiscard]] static size_t mode_index(DrawMode mode) noexcept {
        return static_cast<size_t>(mode);
    }

    /// Recomputes the texel of every palette index in every mode.
    void rebuild_colours() noexcept;

    /// Finds or makes room for a slot on a live page and takes it.
    ///
    /// @param slot_width cell width in texels, a multiple of cell_alignment
    /// @param slot_height cell height in texels, a multiple of cell_alignment
    /// @param entry the entry the slot is for
    /// @param[out] placement the page and the slot's top-left corner
    /// @return false when no live page has room
    bool place(uint32_t slot_width, uint32_t slot_height, uint32_t entry, Placement& placement);

    /// Returns the side of the page a slot needs: the ordinary page, or a
    /// larger power of two for a slot that does not fit one.
    ///
    /// @param slot_width cell width in texels
    /// @param slot_height cell height in texels
    /// @return the side, or 0 when the slot exceeds the largest page
    [[nodiscard]] uint32_t page_side_for(uint32_t slot_width, uint32_t slot_height) const noexcept;

    /// Reports whether a page of a side fits under the memory limit now.
    ///
    /// @param side page side in texels
    /// @return true when its bytes and the pages alive stay within the limit
    [[nodiscard]] bool can_add_page(uint32_t side) const noexcept;

    /// Grows the memory limit so that a page of a side fits: to twice the
    /// limit, or as much as the page needs where that is more, held to
    /// largest_memory_limit, when the growth hooks allow the bytes it adds.
    ///
    /// @param side page side in texels
    /// @return true when the limit grew and the page fits
    bool grow_for(uint32_t side);

    /// Reports whether an entry is held: used by the frame under way.
    ///
    /// @param entry the entry
    /// @return true when it was placed or found since the last begin_frame
    [[nodiscard]] bool held(uint32_t entry) const noexcept {
        return frame_serial_ != 0 && entries_[entry].used_in == frame_serial_;
    }

    /// Makes an empty page, taking a released page's index when there is one.
    ///
    /// @param side page side in texels
    /// @return the page's index
    uint32_t add_page(uint32_t side);

    /// Releases a page that holds no frame.
    ///
    /// @param page index of the page
    void release_page(uint32_t page) noexcept;

    /// Returns an entry's slot to its page, releasing the page when it empties.
    ///
    /// @param entry the entry whose slot is freed
    void release_slot(uint32_t entry) noexcept;

    /// Takes an entry off the pages: its index, its order and its slot.
    ///
    /// @param entry the entry to remove
    void remove_entry(uint32_t entry) noexcept;

    /// Takes an unused entry.
    ///
    /// @return the entry's index
    uint32_t new_entry();

    /// Makes an entry the most recently used, and held by the frame under
    /// way.
    ///
    /// @param entry the entry to link
    void link_newest(uint32_t entry) noexcept;

    /// Takes an entry out of the least-recently-used order.
    ///
    /// @param entry the entry to unlink
    void unlink(uint32_t entry) noexcept;

    /// Writes a frame's texels into its slot, the gutter transparent.
    ///
    /// @param entry the entry whose slot is written
    /// @param source the frame as the GAF reader renders it
    void write_slot(uint32_t entry, const formats::gaf::RenderedFrame& source) noexcept;

    /// Counts a refusal.
    ///
    /// @param status why the request was refused
    /// @return the result carrying the status
    FrameResult refuse(FrameStatus status) noexcept;

    Limits limits_{};
    size_t made_memory_limit_{}; ///< the memory limit the pages were made with
    GrowthHooks growth_{};
    uint64_t frame_serial_{}; ///< frames begun; 0 before the first
    Palette palette_{};
    float gamma_{};
    std::array<uint8_t, gray_table_entries> gray_table_{};
    bool has_palette_{};
    bool has_gray_table_{};
    uint64_t palette_generation_{};
    std::array<ModeColours, draw_mode_count> colours_{};
    std::vector<Page> pages_;
    std::vector<PageLayout> layouts_;
    std::vector<uint32_t> released_pages_;
    std::vector<Entry> entries_;
    std::vector<uint32_t> free_entries_;
    std::unordered_map<uint64_t, std::array<uint32_t, draw_mode_count>> index_;
    uint32_t newest_{none};
    uint32_t oldest_{none};
    size_t page_bytes_{};
    size_t frame_bytes_{};
    uint32_t pages_alive_{};
    uint32_t frames_alive_{};
    Statistics statistics_{};
};

} // namespace oa::present::gpu_world
