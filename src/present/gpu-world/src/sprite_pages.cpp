// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/gpu_world/sprite_pages.hpp"

#include "oa/present/palette_tables.hpp"
#include "oa/present/rle.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace oa::present::gpu_world {
namespace {

/// A colour channel's whole brightness.
constexpr size_t white_level = 255;

/// Brings a page side inside min_page_size..max_page_size and rounds it up
/// to a power of two.
///
/// @param side side asked for, in texels
/// @return the side a page may have
[[nodiscard]] uint32_t normalised_page_side(uint32_t side) noexcept {
    return std::bit_ceil(std::clamp(side, min_page_size, max_page_size));
}

/// Returns the side of a frame's cell along one axis: the frame with its
/// gutter on both sides, rounded up to the cell alignment.
///
/// @param frame_side frame width or height in texels
/// @return the cell's width or height
[[nodiscard]] uint32_t cell_side(uint32_t frame_side) noexcept {
    const uint32_t with_gutter = frame_side + 2 * frame_gutter;
    return (with_gutter + cell_alignment - 1) / cell_alignment * cell_alignment;
}

/// Returns the bytes of a square page's texels.
///
/// @param side page side in texels
/// @return side * side * texel_bytes
[[nodiscard]] size_t page_texel_bytes(uint32_t side) noexcept {
    return size_t{side} * side * texel_bytes;
}

/// Returns the bytes of a slot's texels.
///
/// @param slot a slot on a page
/// @return its area times texel_bytes
[[nodiscard]] size_t slot_texel_bytes(const TexelRect& slot) noexcept {
    return size_t{slot.width} * slot.height * texel_bytes;
}

/// Grows a dirty rectangle to hold another.
///
/// @param[in,out] dirty rectangle to grow; empty takes the other whole
/// @param added rectangle to cover
void cover(TexelRect& dirty, const TexelRect& added) noexcept {
    if (added.empty())
        return;
    if (dirty.empty()) {
        dirty = added;
        return;
    }
    const uint32_t left = std::min(dirty.x, added.x);
    const uint32_t top = std::min(dirty.y, added.y);
    const uint32_t right = std::max(dirty.x + dirty.width, added.x + added.width);
    const uint32_t bottom = std::max(dirty.y + dirty.height, added.y + added.height);
    dirty = {
        static_cast<uint16_t>(left),
        static_cast<uint16_t>(top),
        static_cast<uint16_t>(right - left),
        static_cast<uint16_t>(bottom - top)
    };
}

} // namespace

SpritePages::SpritePages(const Limits& limits) : limits_(limits) {
    limits_.page_size = normalised_page_side(limits.page_size);
    limits_.largest_page_size =
        std::max(limits_.page_size, normalised_page_side(limits.largest_page_size));
    limits_.largest_memory_limit = std::max(limits.memory_limit, limits.largest_memory_limit);
    made_memory_limit_ = limits.memory_limit;
}

void SpritePages::set_palette(const Palette& palette, float gamma) {
    if (has_palette_ && gamma == gamma_ && std::memcmp(&palette, &palette_, sizeof(Palette)) == 0)
        return;
    palette_ = palette;
    gamma_ = gamma;
    has_palette_ = true;
    ++palette_generation_;
    rebuild_colours();
    clear();
}

void SpritePages::set_gray_table(std::span<const uint8_t, gray_table_entries> table) {
    if (has_gray_table_ && std::equal(table.begin(), table.end(), gray_table_.begin()))
        return;
    std::copy(table.begin(), table.end(), gray_table_.begin());
    has_gray_table_ = true;
    rebuild_colours();
    // The greyed records' texels would change: drop them.
    for (uint32_t entry = 0; entry < entries_.size(); ++entry) {
        if (entries_[entry].live && entries_[entry].record.mode == DrawMode::greyed)
            remove_entry(entry);
    }
}

void SpritePages::rebuild_colours() noexcept {
    for (size_t index = 0; index < OA_PALETTE_COLORS; ++index) {
        const PaletteEntry& entry = palette_.entries[index];
        const Texel device{
            gamma_channel(entry.r, gamma_),
            gamma_channel(entry.g, gamma_),
            gamma_channel(entry.b, gamma_),
            opaque_alpha
        };
        colours_[mode_index(DrawMode::opaque)][index] = device;
        const PaletteEntry& gray = palette_.entries[gray_table_[index]];
        colours_[mode_index(DrawMode::greyed)][index] = {
            gamma_channel(gray.r, gamma_),
            gamma_channel(gray.g, gamma_),
            gamma_channel(gray.b, gamma_),
            opaque_alpha
        };
        // A light row's share of white, rounded to the nearest level.
        Texel light{};
        if (index >= static_cast<size_t>(shade_ramp_base) &&
            index - static_cast<size_t>(shade_ramp_base) < static_cast<size_t>(ramp_table_rows)) {
            const size_t row = index - static_cast<size_t>(shade_ramp_base);
            const size_t level = std::min<size_t>(
                white_level,
                (row * white_level + light_rows_per_doubling / 2) / light_rows_per_doubling
            );
            light = {
                static_cast<uint8_t>(level),
                static_cast<uint8_t>(level),
                static_cast<uint8_t>(level),
                0
            };
        }
        colours_[mode_index(DrawMode::lit)][index] = light;
    }
}

FrameResult SpritePages::refuse(FrameStatus status) noexcept {
    ++statistics_.refusals;
    return FrameResult{status, FrameRecord{}};
}

FrameResult
SpritePages::frame(uint64_t frame_id, DrawMode mode, const formats::gaf::RenderedFrame& source) {
    if (!has_palette_)
        return refuse(FrameStatus::no_palette);
    if (mode == DrawMode::greyed && !has_gray_table_)
        return refuse(FrameStatus::no_gray_table);
    if (const auto found = index_.find(frame_id);
        found != index_.end() && found->second[mode_index(mode)] != none) {
        const uint32_t entry = found->second[mode_index(mode)];
        unlink(entry);
        link_newest(entry);
        ++statistics_.hits;
        return FrameResult{FrameStatus::ok, entries_[entry].record};
    }
    if (source.width == 0 || source.height == 0)
        return refuse(FrameStatus::empty);
    const size_t pixel_count = size_t{source.width} * source.height;
    if (source.pixels.size() != pixel_count || source.coverage.size() != pixel_count)
        return refuse(FrameStatus::malformed);
    const uint32_t slot_width = cell_side(source.width);
    const uint32_t slot_height = cell_side(source.height);
    const uint32_t side = page_side_for(slot_width, slot_height);
    if (side == 0)
        return refuse(FrameStatus::too_large);
    // A page no limit the pages may reach allows is refused before anything
    // is evicted for it.
    const size_t reachable =
        growth_.allow != nullptr ? limits_.largest_memory_limit : limits_.memory_limit;
    if (page_texel_bytes(side) > reachable)
        return refuse(FrameStatus::no_room);

    const uint32_t entry = new_entry();
    Placement placement;
    while (!place(slot_width, slot_height, entry, placement)) {
        if (can_add_page(side)) {
            add_page(side);
            continue;
        }
        // The least recently used entry is held only where every entry is:
        // room comes from the frames of earlier frames, else from a larger
        // limit, which a page larger than the limit itself needs whatever
        // is evicted.
        if (page_texel_bytes(side) <= limits_.memory_limit && oldest_ != none && !held(oldest_)) {
            remove_entry(oldest_);
            ++statistics_.evictions;
            continue;
        }
        if (grow_for(side)) {
            add_page(side);
            continue;
        }
        free_entries_.push_back(entry);
        if (oldest_ != none)
            ++statistics_.held_refusals;
        return refuse(FrameStatus::no_room);
    }

    Entry& placed = entries_[entry];
    placed.frame_id = frame_id;
    placed.slot = {
        placement.x,
        placement.y,
        static_cast<uint16_t>(slot_width),
        static_cast<uint16_t>(slot_height)
    };
    placed.record.page = placement.page;
    placed.record.rect = {
        static_cast<uint16_t>(placement.x + frame_gutter),
        static_cast<uint16_t>(placement.y + frame_gutter),
        source.width,
        source.height
    };
    placed.record.origin_x = source.origin_x;
    placed.record.origin_y = source.origin_y;
    placed.record.mode = mode;
    placed.live = true;
    link_newest(entry);
    std::array<uint32_t, draw_mode_count> no_modes{};
    no_modes.fill(none);
    const auto held = index_.try_emplace(frame_id, no_modes).first;
    held->second[mode_index(mode)] = entry;
    write_slot(entry, source);
    frame_bytes_ += slot_texel_bytes(placed.slot);
    ++frames_alive_;
    ++statistics_.decodes;
    return FrameResult{FrameStatus::ok, placed.record};
}

FrameResult SpritePages::find(uint64_t frame_id, DrawMode mode) {
    const auto found = index_.find(frame_id);
    if (found == index_.end() || found->second[mode_index(mode)] == none)
        return FrameResult{FrameStatus::not_held, FrameRecord{}};
    const uint32_t entry = found->second[mode_index(mode)];
    unlink(entry);
    link_newest(entry);
    ++statistics_.hits;
    return FrameResult{FrameStatus::ok, entries_[entry].record};
}

bool SpritePages::holds(uint64_t frame_id, DrawMode mode) const noexcept {
    const auto found = index_.find(frame_id);
    return found != index_.end() && found->second[mode_index(mode)] != none;
}

void SpritePages::forget(uint64_t frame_id) {
    const auto found = index_.find(frame_id);
    if (found == index_.end())
        return;
    const std::array<uint32_t, draw_mode_count> modes = found->second;
    for (const uint32_t entry : modes) {
        if (entry != none)
            remove_entry(entry);
    }
}

void SpritePages::clear() {
    for (uint32_t entry = 0; entry < entries_.size(); ++entry) {
        if (entries_[entry].live)
            remove_entry(entry);
    }
}

std::span<const Page> SpritePages::pages() const noexcept {
    return pages_;
}

void SpritePages::clear_dirty(uint32_t page) noexcept {
    if (page < pages_.size())
        pages_[page].dirty = {};
}

MemoryUse SpritePages::memory() const noexcept {
    MemoryUse use;
    use.page_bytes = page_bytes_;
    use.frame_bytes = frame_bytes_;
    use.memory_limit = limits_.memory_limit;
    use.pages = pages_alive_;
    use.frames = frames_alive_;
    return use;
}

uint32_t SpritePages::page_side_for(uint32_t slot_width, uint32_t slot_height) const noexcept {
    const uint32_t longest = std::max(slot_width, slot_height);
    if (longest <= limits_.page_size)
        return limits_.page_size;
    if (longest > limits_.largest_page_size)
        return 0;
    return std::bit_ceil(longest);
}

bool SpritePages::can_add_page(uint32_t side) const noexcept {
    const size_t bytes = page_texel_bytes(side);
    return bytes <= limits_.memory_limit && page_bytes_ <= limits_.memory_limit - bytes;
}

bool SpritePages::grow_for(uint32_t side) {
    const size_t needed = page_bytes_ + page_texel_bytes(side);
    if (needed > limits_.largest_memory_limit)
        return false;
    const size_t doubled = limits_.memory_limit <= limits_.largest_memory_limit / 2
                               ? limits_.memory_limit * 2
                               : limits_.largest_memory_limit;
    const size_t grown = std::max(doubled, needed);
    if (growth_.allow == nullptr || !growth_.allow(growth_.context, grown - limits_.memory_limit))
        return false;
    limits_.memory_limit = grown;
    ++statistics_.growths;
    return true;
}

uint32_t SpritePages::add_page(uint32_t side) {
    uint32_t page = none;
    if (!released_pages_.empty()) {
        // The lowest released index first, so that the pages stay compact.
        const auto lowest = std::min_element(released_pages_.begin(), released_pages_.end());
        page = *lowest;
        released_pages_.erase(lowest);
    } else {
        page = static_cast<uint32_t>(pages_.size());
        pages_.emplace_back();
        layouts_.emplace_back();
    }
    Page& made = pages_[page];
    made.size = side;
    made.texels.assign(page_texel_bytes(side), 0);
    made.dirty = {};
    ++made.revision;
    layouts_[page] = PageLayout{};
    page_bytes_ += page_texel_bytes(side);
    ++pages_alive_;
    return page;
}

void SpritePages::release_page(uint32_t page) noexcept {
    Page& released = pages_[page];
    page_bytes_ -= page_texel_bytes(released.size);
    released.size = 0;
    released.texels = std::vector<uint8_t>{};
    released.dirty = {};
    ++released.revision;
    layouts_[page] = PageLayout{};
    released_pages_.push_back(page);
    --pages_alive_;
}

bool SpritePages::place(
    uint32_t slot_width, uint32_t slot_height, uint32_t entry, Placement& placement
) {
    // Every slot side is a multiple of cell_alignment, a page's side a power
    // of two, and a shelf starts at 0 or after whole slots, so shelf rows,
    // open space and split remainders keep the alignment without rounding
    // here.
    // The shelf that wastes the fewest rows over the slot, in a free slot or
    // in its open space; and the first page with room for a new shelf.
    uint32_t best_waste = std::numeric_limits<uint32_t>::max();
    uint32_t best_page = none;
    size_t best_shelf = 0;
    size_t best_slot = 0;
    bool best_open = false;
    uint32_t shelf_page = none;
    for (uint32_t page = 0; page < pages_.size(); ++page) {
        const uint32_t size = pages_[page].size;
        if (size == 0)
            continue;
        const PageLayout& layout = layouts_[page];
        for (size_t shelf_index = 0; shelf_index < layout.shelves.size(); ++shelf_index) {
            const Shelf& shelf = layout.shelves[shelf_index];
            if (shelf.height < slot_height)
                continue;
            const uint32_t waste = shelf.height - slot_height;
            if (waste >= best_waste)
                continue;
            // The first free run wide enough, else the shelf's open space.
            const auto free_run =
                std::find_if(shelf.slots.begin(), shelf.slots.end(), [&](const Slot& slot) {
                    return slot.entry == none && slot.width >= slot_width;
                });
            const bool open_fits = size - shelf.open_x >= slot_width;
            if (free_run == shelf.slots.end() && !open_fits)
                continue;
            best_waste = waste;
            best_page = page;
            best_shelf = shelf_index;
            best_open = free_run == shelf.slots.end();
            best_slot = best_open ? 0 : static_cast<size_t>(free_run - shelf.slots.begin());
        }
        if (shelf_page == none && size >= slot_width && size - layout.next_shelf_y >= slot_height)
            shelf_page = page;
    }

    const bool reuse_shelf = best_page != none && (shelf_page == none ||
                                                   best_waste <= slot_height / shelf_waste_divisor);
    if (reuse_shelf) {
        Shelf& shelf = layouts_[best_page].shelves[best_shelf];
        placement.page = best_page;
        placement.y = shelf.y;
        if (best_open) {
            placement.x = shelf.open_x;
            shelf.slots.push_back(Slot{shelf.open_x, static_cast<uint16_t>(slot_width), entry});
            shelf.open_x = static_cast<uint16_t>(shelf.open_x + slot_width);
            return true;
        }
        Slot& slot = shelf.slots[best_slot];
        placement.x = slot.x;
        if (slot.width > slot_width) {
            const Slot remainder{
                static_cast<uint16_t>(slot.x + slot_width),
                static_cast<uint16_t>(slot.width - slot_width),
                none
            };
            slot.width = static_cast<uint16_t>(slot_width);
            slot.entry = entry;
            shelf.slots.insert(
                shelf.slots.begin() + static_cast<std::ptrdiff_t>(best_slot) + 1, remainder
            );
            return true;
        }
        slot.entry = entry;
        return true;
    }
    if (shelf_page == none)
        return false;
    PageLayout& layout = layouts_[shelf_page];
    Shelf shelf;
    shelf.y = layout.next_shelf_y;
    shelf.height = static_cast<uint16_t>(slot_height);
    shelf.open_x = static_cast<uint16_t>(slot_width);
    shelf.slots.push_back(Slot{0, static_cast<uint16_t>(slot_width), entry});
    placement.page = shelf_page;
    placement.x = 0;
    placement.y = shelf.y;
    layout.next_shelf_y = static_cast<uint16_t>(layout.next_shelf_y + slot_height);
    layout.shelves.push_back(std::move(shelf));
    return true;
}

void SpritePages::release_slot(uint32_t entry) noexcept {
    const Entry& freed = entries_[entry];
    PageLayout& layout = layouts_[freed.record.page];
    auto& shelves = layout.shelves;
    const auto shelf_found = std::find_if(shelves.begin(), shelves.end(), [&](const Shelf& shelf) {
        return shelf.y == freed.slot.y;
    });
    if (shelf_found == shelves.end())
        return;
    Shelf& shelf = *shelf_found;
    auto& slots = shelf.slots;
    const auto slot_found = std::find_if(slots.begin(), slots.end(), [&](const Slot& slot) {
        return slot.x == freed.slot.x && slot.entry == entry;
    });
    if (slot_found == slots.end())
        return;
    size_t index = static_cast<size_t>(slot_found - slots.begin());
    slots[index].entry = none;
    // Free neighbours join into one run.
    if (index + 1 < slots.size() && slots[index + 1].entry == none) {
        slots[index].width = static_cast<uint16_t>(slots[index].width + slots[index + 1].width);
        slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(index) + 1);
    }
    if (index > 0 && slots[index - 1].entry == none) {
        slots[index - 1].width = static_cast<uint16_t>(slots[index - 1].width + slots[index].width);
        slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(index));
        --index;
    }
    // A free run at the end of the shelf becomes open space again.
    if (index + 1 == slots.size()) {
        shelf.open_x = slots[index].x;
        slots.pop_back();
    }
    // Empty shelves at the bottom give their rows back to the page.
    while (!shelves.empty() && shelves.back().slots.empty()) {
        layout.next_shelf_y = shelves.back().y;
        shelves.pop_back();
    }
    if (shelves.empty())
        release_page(freed.record.page);
}

void SpritePages::remove_entry(uint32_t entry) noexcept {
    Entry& removed = entries_[entry];
    if (const auto found = index_.find(removed.frame_id); found != index_.end()) {
        found->second[mode_index(removed.record.mode)] = none;
        if (std::all_of(found->second.begin(), found->second.end(), [](uint32_t held) {
                return held == none;
            }))
            index_.erase(found);
    }
    unlink(entry);
    release_slot(entry);
    frame_bytes_ -= slot_texel_bytes(removed.slot);
    --frames_alive_;
    removed.live = false;
    free_entries_.push_back(entry);
}

uint32_t SpritePages::new_entry() {
    if (!free_entries_.empty()) {
        const uint32_t entry = free_entries_.back();
        free_entries_.pop_back();
        entries_[entry] = Entry{};
        return entry;
    }
    entries_.emplace_back();
    return static_cast<uint32_t>(entries_.size() - 1);
}

void SpritePages::link_newest(uint32_t entry) noexcept {
    Entry& linked = entries_[entry];
    linked.used_in = frame_serial_;
    linked.newer = none;
    linked.older = newest_;
    if (newest_ != none)
        entries_[newest_].newer = entry;
    newest_ = entry;
    if (oldest_ == none)
        oldest_ = entry;
}

void SpritePages::unlink(uint32_t entry) noexcept {
    Entry& unlinked = entries_[entry];
    if (unlinked.newer != none)
        entries_[unlinked.newer].older = unlinked.older;
    else if (newest_ == entry)
        newest_ = unlinked.older;
    if (unlinked.older != none)
        entries_[unlinked.older].newer = unlinked.newer;
    else if (oldest_ == entry)
        oldest_ = unlinked.newer;
    unlinked.newer = none;
    unlinked.older = none;
}

void SpritePages::write_slot(uint32_t entry, const formats::gaf::RenderedFrame& source) noexcept {
    const Entry& written = entries_[entry];
    Page& page = pages_[written.record.page];
    const size_t row_bytes = size_t{page.size} * texel_bytes;
    uint8_t* const texels = page.texels.data();
    const TexelRect& slot = written.slot;
    // The whole slot transparent first: the gutter, and the texels the frame
    // does not cover.
    for (uint32_t row = 0; row < slot.height; ++row) {
        std::memset(
            texels + (size_t{slot.y} + row) * row_bytes + size_t{slot.x} * texel_bytes,
            0,
            size_t{slot.width} * texel_bytes
        );
    }
    const ModeColours& colours = colours_[mode_index(written.record.mode)];
    const TexelRect& rect = written.record.rect;
    // Plain pointers keep the per-texel loop free of calls in unoptimised builds.
    const uint8_t* const pixels = source.pixels.data();
    const uint8_t* const coverage = source.coverage.data();
    for (uint32_t y = 0; y < rect.height; ++y) {
        uint8_t* out = texels + (size_t{rect.y} + y) * row_bytes + size_t{rect.x} * texel_bytes;
        const size_t source_row = size_t{y} * rect.width;
        for (uint32_t x = 0; x < rect.width; ++x, out += texel_bytes) {
            const size_t offset = source_row + x;
            if (coverage[offset] == 0)
                continue;
            const Texel& texel = colours[pixels[offset]];
            std::memcpy(out, texel.data(), texel_bytes);
        }
    }
    cover(page.dirty, slot);
    ++page.revision;
}

} // namespace oa::present::gpu_world
