// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Running card command lists on SDL's renderer.
#include "oa/app/card/executor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>

namespace oa::app::card {

namespace {

// The renderer reads a vertex's colour as four floats in the order Colour
// holds them.
static_assert(sizeof(Colour) == sizeof(SDL_FColor));
static_assert(offsetof(Colour, red) == offsetof(SDL_FColor, r));
static_assert(offsetof(Colour, green) == offsetof(SDL_FColor, g));
static_assert(offsetof(Colour, blue) == offsetof(SDL_FColor, b));
static_assert(offsetof(Colour, alpha) == offsetof(SDL_FColor, a));

/// The format of a page's textures: four bytes a texel, red, green, blue
/// and alpha in that order in memory, as Texel holds them, so that a page
/// is uploaded as the terrain and sprite pages hold it.
constexpr SDL_PixelFormat page_format = SDL_PIXELFORMAT_RGBA32;

/// The format of a render target's textures, one every renderer holds.
constexpr SDL_PixelFormat target_format = SDL_PIXELFORMAT_ARGB8888;

/// The colour a new render target is cleared to and read back as: one a
/// texture the driver failed to make, which reads as black, cannot return.
constexpr std::array<uint8_t, 3> target_check_colour{0x5A, 0xA5, 0x3C};

/// The name SDL gives its software renderer, whose pages are streaming
/// textures: it run-length encodes a static texture, and a copy of a part
/// of one then scans the whole.
constexpr std::string_view software_renderer_name = "software";

/// A handle's low bits hold its slot, one more than the index, and its high
/// bits the slot's generation when it was handed out.
constexpr uint32_t handle_slot_bits = 16;
constexpr uint32_t handle_slot_mask = (1U << handle_slot_bits) - 1U;

/// Makes a handle's value.
///
/// @param slot the slot's index
/// @param generation the slot's generation
/// @return the value
uint32_t make_handle(std::size_t slot, uint32_t generation) noexcept {
    return ((generation & handle_slot_mask) << handle_slot_bits) |
           (static_cast<uint32_t>(slot) + 1U);
}

/// Splits a handle's value.
///
/// @param value the value
/// @param[out] slot the slot's index
/// @param[out] generation the generation it was handed out at
/// @return false for the value that names nothing
bool split_handle(uint32_t value, std::size_t& slot, uint32_t& generation) noexcept {
    if ((value & handle_slot_mask) == 0)
        return false;
    slot = (value & handle_slot_mask) - 1U;
    generation = value >> handle_slot_bits;
    return true;
}

/// Writes a size as "WxH".
///
/// @param width across
/// @param height down
/// @return the text
std::string size_text(uint64_t width, uint64_t height) {
    return std::to_string(width) + "x" + std::to_string(height);
}

/// Says whether two rectangles are the same.
///
/// @param first one
/// @param second the other
/// @return true when every field agrees
bool same_rect(const Rect& first, const Rect& second) noexcept {
    return first.x == second.x && first.y == second.y && first.width == second.width &&
           first.height == second.height;
}

/// Says whether a draw batch may run in the geometry call of the one
/// before it: the same target, page, level, blend, sampling and scissor,
/// and indices that follow on.
///
/// @param first the batch before, or the run of batches so far
/// @param second the batch after
/// @param next_index the index the run so far ends at
/// @return true when the second may join
bool mergeable(const Batch& first, const Batch& second, Index next_index) noexcept {
    return second.operation == Operation::draw && second.target == first.target &&
           second.page == first.page && second.level == first.level &&
           second.blend == first.blend && second.sampling == first.sampling &&
           second.scissored == first.scissored &&
           (!first.scissored || same_rect(first.scissor, second.scissor)) &&
           second.first_index == next_index;
}

/// Bytes of a texture of a size, four a texel.
///
/// @param width across
/// @param height down
/// @return the bytes
uint64_t texture_bytes_of(uint64_t width, uint64_t height) noexcept {
    return width * height * texel_bytes;
}

} // namespace

bool Executor::fail(const std::string& what) {
    error_ = what + ": " + SDL_GetError();
    return false;
}

bool Executor::refuse(const std::string& why) {
    error_ = why;
    return false;
}

bool Executor::open(SDL_Renderer* renderer, uint32_t texture_limit) {
    if (renderer_ != nullptr)
        close();
    if (renderer == nullptr)
        return refuse("no renderer to run card frames on");
    renderer_ = renderer;
    capabilities_ = {};
    counts_ = {};
    capabilities_.texture_limit = texture_limit;
    const char* name = SDL_GetRendererName(renderer);
    page_access_ = name != nullptr && std::string_view(name) == software_renderer_name
                       ? SDL_TEXTUREACCESS_STREAMING
                       : SDL_TEXTUREACCESS_STATIC;
#if SDL_VERSION_ATLEAST(3, 4, 0)
    capabilities_.clamped_addressing = SDL_SetRenderTextureAddressMode(
        renderer, SDL_TEXTURE_ADDRESS_CLAMP, SDL_TEXTURE_ADDRESS_CLAMP
    );
#endif
    // Darken: what is under the draw times one less the source alpha, the
    // source colour unread.
    darken_mode_ = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD
    );
    // Minimum: each channel the lesser of the source's and the
    // destination's, the destination's alpha kept.
    minimum_mode_ = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_MINIMUM,
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD
    );
    SDL_Texture* probe = SDL_CreateTexture(renderer, page_format, SDL_TEXTUREACCESS_STATIC, 1, 1);
    if (probe == nullptr) {
        renderer_ = nullptr;
        return fail("SDL_CreateTexture of the blend probe");
    }
    capabilities_.darken_composed = SDL_SetTextureBlendMode(probe, darken_mode_);
    capabilities_.minimum_composed = SDL_SetTextureBlendMode(probe, minimum_mode_);
#if SDL_VERSION_ATLEAST(3, 4, 0)
    capabilities_.pixel_art_sampling = SDL_SetTextureScaleMode(probe, SDL_SCALEMODE_PIXELART);
#endif
    SDL_DestroyTexture(probe);
    // A refusal of the composed mode or the sampling mode is answered by
    // the fallback, not an error.
    SDL_ClearError();
    return true;
}

void Executor::close() noexcept {
    for (std::size_t slot = 0; slot < pages_.size(); ++slot)
        if (pages_[slot].alive)
            free_page(slot);
    for (std::size_t slot = 0; slot < targets_.size(); ++slot)
        if (targets_[slot].alive)
            free_target(slot);
    pages_.clear();
    free_pages_.clear();
    retired_pages_.clear();
    targets_.clear();
    free_targets_.clear();
    scratch_.clear();
    renderer_ = nullptr;
}

const Executor::Page* Executor::find_page(PageHandle page) const noexcept {
    std::size_t slot = 0;
    uint32_t generation = 0;
    if (!split_handle(page.value, slot, generation) || slot >= pages_.size())
        return nullptr;
    const Page& found = pages_[slot];
    if (!found.alive || (found.generation & handle_slot_mask) != generation)
        return nullptr;
    return &found;
}

Executor::Page* Executor::find_page(PageHandle page) noexcept {
    return const_cast<Page*>(static_cast<const Executor*>(this)->find_page(page));
}

const Executor::Target* Executor::find_target(TargetHandle target) const noexcept {
    std::size_t slot = 0;
    uint32_t generation = 0;
    if (!split_handle(target.value, slot, generation) || slot >= targets_.size())
        return nullptr;
    const Target& found = targets_[slot];
    if (!found.alive || (found.generation & handle_slot_mask) != generation)
        return nullptr;
    return &found;
}

Executor::Target* Executor::find_target(TargetHandle target) noexcept {
    return const_cast<Target*>(static_cast<const Executor*>(this)->find_target(target));
}

PageHandle Executor::create_page(const PageDescription& description) {
    if (!is_open()) {
        refuse("the executor is not open");
        return {};
    }
    const std::string size = size_text(description.width, description.height);
    if (description.width == 0 || description.height == 0) {
        refuse("a page of " + size + " texels has no texels");
        return {};
    }
    if (description.width > largest_page_edge || description.height > largest_page_edge) {
        refuse(
            "a page of " + size + " texels is beyond the largest page edge of " +
            std::to_string(largest_page_edge)
        );
        return {};
    }
    const uint32_t limit = capabilities_.texture_limit;
    if (limit != 0 && (description.width > limit || description.height > limit)) {
        refuse(
            "a page of " + size + " texels is beyond the renderer's texture limit of " +
            std::to_string(limit)
        );
        return {};
    }
    if (description.level_count == 0 || description.level_count > most_page_levels) {
        refuse(
            "a page of " + std::to_string(description.level_count) + " levels; a page has 1 to " +
            std::to_string(most_page_levels)
        );
        return {};
    }
    if (counts_.pages_alive >= most_pages) {
        refuse(std::to_string(most_pages) + " pages are alive already");
        return {};
    }
    std::size_t slot = 0;
    if (!free_pages_.empty()) {
        slot = free_pages_.back();
        free_pages_.pop_back();
    } else {
        slot = pages_.size();
        pages_.emplace_back();
    }
    Page& page = pages_[slot];
    page.level_count = description.level_count;
    uint64_t bytes = 0;
    for (uint8_t level = 0; level < description.level_count; ++level) {
        Level& made = page.levels[level];
        made.width = level_edge(description.width, level);
        made.height = level_edge(description.height, level);
        made.blend = SDL_BLENDMODE_NONE;
        made.scale = SDL_SCALEMODE_NEAREST;
        made.texture = SDL_CreateTexture(
            renderer_,
            page_format,
            page_access_,
            static_cast<int>(made.width),
            static_cast<int>(made.height)
        );
        // SDL blends alpha formats and samples linearly unless told not to;
        // a draw sets both as it needs them.
        const bool ready = made.texture != nullptr &&
                           SDL_SetTextureScaleMode(made.texture, made.scale) &&
                           SDL_SetTextureBlendMode(made.texture, made.blend);
        if (!ready) {
            std::ignore = fail(
                "SDL_CreateTexture of page level " + std::to_string(level) + " (" +
                size_text(made.width, made.height) + ")"
            );
            for (uint8_t undone = 0; undone <= level; ++undone) {
                SDL_DestroyTexture(page.levels[undone].texture);
                page.levels[undone] = {};
            }
            page.level_count = 0;
            free_pages_.push_back(slot);
            return {};
        }
        bytes += texture_bytes_of(made.width, made.height);
    }
    page.alive = true;
    ++counts_.pages_alive;
    counts_.texture_bytes += bytes;
    return PageHandle{make_handle(slot, page.generation)};
}

bool Executor::update_page(
    PageHandle handle, uint8_t level, const Rect* part, const uint8_t* texels, uint32_t pitch
) {
    Page* page = find_page(handle);
    if (page == nullptr)
        return refuse("page " + std::to_string(handle.value) + " is not alive");
    if (level >= page->level_count)
        return refuse(
            "page " + std::to_string(handle.value) + " has no level " + std::to_string(level)
        );
    const Level& found = page->levels[level];
    const Rect whole{0, 0, static_cast<int32_t>(found.width), static_cast<int32_t>(found.height)};
    const Rect& written = part != nullptr ? *part : whole;
    const bool inside = written.x >= 0 && written.y >= 0 && written.width > 0 &&
                        written.height > 0 &&
                        int64_t{written.x} + written.width <= int64_t{found.width} &&
                        int64_t{written.y} + written.height <= int64_t{found.height};
    if (!inside)
        return refuse(
            "a part at " + std::to_string(written.x) + "," + std::to_string(written.y) + " of " +
            size_text(static_cast<uint64_t>(written.width), static_cast<uint64_t>(written.height)) +
            " outside level " + std::to_string(level) + " of " +
            size_text(found.width, found.height)
        );
    if (texels == nullptr)
        return refuse("no texels to write");
    const uint64_t row_bytes = static_cast<uint64_t>(written.width) * texel_bytes;
    if (pitch < row_bytes)
        return refuse(
            "a pitch of " + std::to_string(pitch) + " bytes under the " +
            std::to_string(row_bytes) + " of the part's " + std::to_string(written.width) +
            " texels"
        );
    if (pitch > static_cast<uint32_t>(std::numeric_limits<int>::max()))
        return refuse("a pitch of " + std::to_string(pitch) + " bytes is beyond reach");
    const SDL_Rect rect{written.x, written.y, written.width, written.height};
    if (!SDL_UpdateTexture(found.texture, &rect, texels, static_cast<int>(pitch)))
        return fail("SDL_UpdateTexture of page level " + std::to_string(level));
    return true;
}

void Executor::destroy_page(PageHandle handle) noexcept {
    const Page* page = find_page(handle);
    if (page != nullptr)
        free_page(static_cast<std::size_t>(page - pages_.data()));
}

void Executor::retire_page(PageHandle page) {
    if (find_page(page) != nullptr)
        retired_pages_.push_back(page);
}

bool Executor::page_alive(PageHandle page) const noexcept {
    return find_page(page) != nullptr;
}

SDL_Texture* Executor::page_texture(PageHandle handle, uint8_t level) const noexcept {
    const Page* page = find_page(handle);
    if (page == nullptr || level >= page->level_count)
        return nullptr;
    return page->levels[level].texture;
}

void Executor::free_page(std::size_t slot) noexcept {
    Page& page = pages_[slot];
    for (uint8_t level = 0; level < page.level_count; ++level) {
        Level& made = page.levels[level];
        SDL_DestroyTexture(made.texture);
        counts_.texture_bytes -= texture_bytes_of(made.width, made.height);
        made = {};
    }
    page.level_count = 0;
    page.alive = false;
    ++page.generation;
    --counts_.pages_alive;
    free_pages_.push_back(slot);
}

bool Executor::prepare_target(SDL_Texture* texture, uint32_t factor, const std::string& need) {
    if (!SDL_SetRenderTarget(renderer_, texture) ||
        !SDL_SetRenderScale(renderer_, static_cast<float>(factor), static_cast<float>(factor)) ||
        !SDL_SetRenderDrawColor(
            renderer_, target_check_colour[0], target_check_colour[1], target_check_colour[2], 255
        ) ||
        !SDL_RenderClear(renderer_))
        return fail("SDL clearing " + need);
    // A texture the driver failed to make draws black and reports no
    // error, so one pixel of the clear is read back before the target is
    // trusted.
    const SDL_Rect first_pixel{0, 0, 1, 1};
    SDL_Surface* read = SDL_RenderReadPixels(renderer_, &first_pixel);
    if (read == nullptr)
        return fail("SDL_RenderReadPixels of " + need);
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    uint8_t alpha = 0;
    const bool readable = SDL_ReadSurfacePixel(read, 0, 0, &red, &green, &blue, &alpha);
    SDL_DestroySurface(read);
    if (!readable)
        return fail("SDL_ReadSurfacePixel of " + need);
    if (red != target_check_colour[0] || green != target_check_colour[1] ||
        blue != target_check_colour[2])
        return refuse(
            "the one pixel read back from " + need + " is " + std::to_string(red) + "," +
            std::to_string(green) + "," + std::to_string(blue) +
            ", not the colour it was cleared to"
        );
    if (!SDL_SetRenderDrawColorFloat(renderer_, 0.0F, 0.0F, 0.0F, 0.0F) ||
        !SDL_RenderClear(renderer_))
        return fail("SDL clearing " + need + " transparent");
    return true;
}

namespace {

/// Returns how many times a render target at a factor is halved before the
/// one LINEAR draw that resolves it into a target at another factor: until
/// its factor is at most twice that target's, so that the draw reduces it
/// by at most 2 in that target's own pixels. Into the frame's final target,
/// factor 1, that is one fewer than the factor's doublings, none at a
/// factor of 1 or 2; into a target at the source's own factor, none.
///
/// @param factor the supersampling factor, a power of two
/// @param into_factor the factor of the target drawn into, a power of two;
///        1 for the frame's final target
/// @return the halvings
uint32_t resolve_halvings(uint32_t factor, uint32_t into_factor = 1) noexcept {
    uint32_t halvings = 0;
    while ((factor >> halvings) > 2U * into_factor)
        ++halvings;
    return halvings;
}

} // namespace

TargetHandle
Executor::create_target(uint32_t width, uint32_t height, uint32_t factor, bool keep_half) {
    if (!is_open()) {
        refuse("the executor is not open");
        return {};
    }
    const bool power_of_two = factor != 0 && (factor & (factor - 1)) == 0;
    if (!power_of_two || factor > largest_supersampling_factor) {
        refuse(
            "a supersampling factor of " + std::to_string(factor) +
            "; a render target takes a power of two from 1 to " +
            std::to_string(largest_supersampling_factor)
        );
        return {};
    }
    if (width == 0 || height == 0) {
        refuse("a render target of " + size_text(width, height) + " pixels has no pixels");
        return {};
    }
    const uint64_t texture_width = uint64_t{width} * factor;
    const uint64_t texture_height = uint64_t{height} * factor;
    const std::string need = "a render target of " + size_text(width, height) + " at factor " +
                             std::to_string(factor) + " needs a texture of " +
                             size_text(texture_width, texture_height);
    if (texture_width > largest_target_edge || texture_height > largest_target_edge) {
        refuse(need + ", beyond the largest target edge of " + std::to_string(largest_target_edge));
        return {};
    }
    const uint32_t limit = capabilities_.texture_limit;
    if (limit != 0 && (texture_width > limit || texture_height > limit)) {
        refuse(need + ", beyond the renderer's texture limit of " + std::to_string(limit));
        return {};
    }
    if (counts_.targets_alive >= most_targets) {
        refuse(std::to_string(most_targets) + " render targets are alive already");
        return {};
    }
    std::size_t slot = 0;
    if (!free_targets_.empty()) {
        slot = free_targets_.back();
        free_targets_.pop_back();
    } else {
        slot = targets_.size();
        targets_.emplace_back();
    }
    Target& target = targets_[slot];
    target.width = width;
    target.height = height;
    target.factor = factor;
    target.blend = SDL_BLENDMODE_NONE;
    target.half_blends.fill(SDL_BLENDMODE_NONE);
    target.halves.fill(nullptr);
    target.halved_in = 0;
    target.halved_count = 0;
    target.clip = {true, false, {}};
    // The target is made, set to reduce LINEAR, scaled to its factor,
    // cleared and read back, then cleared transparent; a factor above 2,
    // and a target that keeps its half, also make the halves it reduces
    // through. A failure undoes it all, with error() already set.
    const auto undo = [&]() {
        SDL_DestroyTexture(target.texture);
        for (SDL_Texture* half : target.halves)
            SDL_DestroyTexture(half);
        const uint32_t generation = target.generation;
        target = {};
        target.generation = generation;
        free_targets_.push_back(slot);
        return TargetHandle{};
    };
    const auto make = [&](uint64_t texture_edge_width, uint64_t texture_edge_height) {
        SDL_Texture* made = SDL_CreateTexture(
            renderer_,
            target_format,
            SDL_TEXTUREACCESS_TARGET,
            static_cast<int>(texture_edge_width),
            static_cast<int>(texture_edge_height)
        );
        if (made != nullptr && !(SDL_SetTextureScaleMode(made, SDL_SCALEMODE_LINEAR) &&
                                 SDL_SetTextureBlendMode(made, SDL_BLENDMODE_NONE))) {
            SDL_DestroyTexture(made);
            made = nullptr;
        }
        return made;
    };
    target.texture = make(texture_width, texture_height);
    if (target.texture == nullptr) {
        std::ignore = fail("SDL_CreateTexture of " + need);
        return undo();
    }
    const uint32_t halvings = std::max<uint32_t>(resolve_halvings(factor), keep_half ? 1U : 0U);
    for (uint32_t level = 0; level < halvings; ++level) {
        target.halves[level] = make(texture_width >> (level + 1), texture_height >> (level + 1));
        if (target.halves[level] == nullptr) {
            std::ignore = fail("SDL_CreateTexture of a half of " + need);
            return undo();
        }
    }
    SDL_Texture* previous = SDL_GetRenderTarget(renderer_);
    SDL_FColor colour{};
    bool ready = SDL_GetRenderDrawColorFloat(renderer_, &colour.r, &colour.g, &colour.b, &colour.a);
    if (!ready)
        std::ignore = fail("SDL_GetRenderDrawColorFloat before preparing " + need);
    ready = ready && prepare_target(target.texture, factor, need);
    for (uint32_t level = 0; ready && level < halvings; ++level)
        ready = prepare_target(target.halves[level], 1, "a half of " + need);
    const bool back =
        SDL_SetRenderTarget(renderer_, previous) &&
        SDL_SetRenderDrawColorFloat(renderer_, colour.r, colour.g, colour.b, colour.a);
    if (!ready)
        return undo();
    if (!back) {
        std::ignore = fail("SDL putting back the target and draw colour after " + need);
        return undo();
    }
    target.alive = true;
    ++counts_.targets_alive;
    counts_.texture_bytes += texture_bytes_of(texture_width, texture_height);
    for (uint32_t level = 0; level < halvings; ++level)
        counts_.texture_bytes +=
            texture_bytes_of(texture_width >> (level + 1), texture_height >> (level + 1));
    return TargetHandle{make_handle(slot, target.generation)};
}

void Executor::destroy_target(TargetHandle handle) noexcept {
    const Target* target = find_target(handle);
    if (target != nullptr)
        free_target(static_cast<std::size_t>(target - targets_.data()));
}

bool Executor::target_alive(TargetHandle target) const noexcept {
    return find_target(target) != nullptr;
}

SDL_Texture* Executor::target_texture(TargetHandle handle) const noexcept {
    const Target* target = find_target(handle);
    return target != nullptr ? target->texture : nullptr;
}

void Executor::free_target(std::size_t slot) noexcept {
    Target& target = targets_[slot];
    const uint64_t texture_width = uint64_t{target.width} * target.factor;
    const uint64_t texture_height = uint64_t{target.height} * target.factor;
    counts_.texture_bytes -= texture_bytes_of(texture_width, texture_height);
    for (uint32_t level = 0; level < most_halvings; ++level)
        if (target.halves[level] != nullptr)
            counts_.texture_bytes -=
                texture_bytes_of(texture_width >> (level + 1), texture_height >> (level + 1));
    SDL_DestroyTexture(target.texture);
    for (SDL_Texture* half : target.halves)
        SDL_DestroyTexture(half);
    const uint32_t generation = target.generation + 1;
    target = {};
    target.generation = generation;
    --counts_.targets_alive;
    free_targets_.push_back(slot);
}

std::string Executor::check_handles(const CardFrame& frame, const SDL_Texture* final_target) const {
    if (final_target != nullptr)
        for (const Target& target : targets_)
            if (target.alive &&
                (target.texture == final_target ||
                 std::find(target.halves.begin(), target.halves.end(), final_target) !=
                     target.halves.end()))
                return "the final target is a render target of the executor's; a resolve draws it";
    const auto target_fault = [&](std::size_t position, TargetHandle handle, const char* role) {
        if (handle == TargetHandle{} || find_target(handle) != nullptr)
            return std::string{};
        return "batch " + std::to_string(position) + ": render target " +
               std::to_string(handle.value) + " (" + role + ") is not alive";
    };
    for (std::size_t position = 0; position < frame.batches.size(); ++position) {
        const Batch& batch = frame.batches[position];
        std::string fault = target_fault(position, batch.target, "drawn into");
        if (fault.empty() &&
            (batch.operation == Operation::resolve || batch.operation == Operation::blend_reduce))
            fault = target_fault(position, batch.source, "the source");
        if (fault.empty() && batch.operation == Operation::blend_reduce) {
            const Target* source = find_target(batch.source);
            const Rect& part = batch.source_part;
            if (source->halves[0] == nullptr)
                fault = "batch " + std::to_string(position) + ": render target " +
                        std::to_string(batch.source.value) +
                        " was made without its half, which a two-level reduction reads";
            else if (
                uint64_t{static_cast<uint32_t>(part.x)} + static_cast<uint32_t>(part.width) >
                    uint64_t{source->width} * source->factor ||
                uint64_t{static_cast<uint32_t>(part.y)} + static_cast<uint32_t>(part.height) >
                    uint64_t{source->height} * source->factor
            )
                fault = "batch " + std::to_string(position) +
                        ": a source part beyond render target " +
                        std::to_string(batch.source.value) + "'s texture of " +
                        size_text(
                            uint64_t{source->width} * source->factor,
                            uint64_t{source->height} * source->factor
                        );
        }
        if (fault.empty() && batch.operation == Operation::draw && batch.page != PageHandle{}) {
            const Page* page = find_page(batch.page);
            if (page == nullptr)
                fault = "batch " + std::to_string(position) + ": page " +
                        std::to_string(batch.page.value) + " is not alive";
            else if (batch.level >= page->level_count)
                fault = "batch " + std::to_string(position) + ": page " +
                        std::to_string(batch.page.value) + " has no level " +
                        std::to_string(batch.level);
        }
        if (!fault.empty())
            return fault;
    }
    return {};
}

bool Executor::bind(Run& run, Target* target) {
    SDL_Texture* texture = target != nullptr ? target->texture : run.final_target;
    if (run.bound == texture && (target != nullptr ? run.bound_target == target : run.final_bound))
        return true;
    if (!SDL_SetRenderTarget(renderer_, texture))
        return fail("SDL_SetRenderTarget");
    run.bound = texture;
    run.bound_target = target;
    run.final_bound = target == nullptr;
    ++counts_.target_switches;
    return true;
}

bool Executor::bind_texture(Run& run, SDL_Texture* texture) {
    if (run.bound == texture && run.bound_target == nullptr && !run.final_bound)
        return true;
    if (!SDL_SetRenderTarget(renderer_, texture))
        return fail("SDL_SetRenderTarget of a render target's half");
    run.bound = texture;
    run.bound_target = nullptr;
    run.final_bound = false;
    ++counts_.target_switches;
    return true;
}

bool Executor::set_clip(Run& run, bool scissored, const Rect& scissor) {
    ClipState* state = run.final_bound               ? &run.final_clip
                       : run.bound_target != nullptr ? &run.bound_target->clip
                                                     : nullptr;
    if (state == nullptr)
        return true;
    if (state->known && state->clipped == scissored &&
        (!scissored || same_rect(state->clip, scissor)))
        return true;
    const SDL_Rect rect{scissor.x, scissor.y, scissor.width, scissor.height};
    if (!SDL_SetRenderClipRect(renderer_, scissored ? &rect : nullptr))
        return fail("SDL_SetRenderClipRect");
    state->known = true;
    state->clipped = scissored;
    state->clip = scissor;
    if (run.final_bound)
        run.final_clip_changed = true;
    ++counts_.clip_changes;
    return true;
}

bool Executor::set_texture_blend(SDL_Texture* texture, SDL_BlendMode& current, SDL_BlendMode mode) {
    if (current == mode)
        return true;
    if (!SDL_SetTextureBlendMode(texture, mode))
        return fail("SDL_SetTextureBlendMode");
    current = mode;
    ++counts_.blend_changes;
    return true;
}

bool Executor::set_texture_sampling(
    SDL_Texture* texture, SDL_ScaleMode& current, SDL_ScaleMode mode
) {
    if (current == mode)
        return true;
    if (!SDL_SetTextureScaleMode(texture, mode))
        return fail("SDL_SetTextureScaleMode");
    current = mode;
    ++counts_.sampling_changes;
    return true;
}

bool Executor::set_draw_blend(Run& run, SDL_BlendMode mode) {
    if (run.draw_blend == mode)
        return true;
    if (!SDL_SetRenderDrawBlendMode(renderer_, mode))
        return fail("SDL_SetRenderDrawBlendMode");
    run.draw_blend = mode;
    run.draw_blend_changed = true;
    ++counts_.blend_changes;
    return true;
}

SDL_BlendMode Executor::blend_mode(Blend blend) const noexcept {
    switch (blend) {
    case Blend::none:
        return SDL_BLENDMODE_NONE;
    case Blend::alpha:
        return SDL_BLENDMODE_BLEND;
    case Blend::alpha_premultiplied:
        return SDL_BLENDMODE_BLEND_PREMULTIPLIED;
    case Blend::additive:
        return SDL_BLENDMODE_ADD;
    case Blend::modulate:
        return SDL_BLENDMODE_MOD;
    case Blend::darken:
        return capabilities_.darken_composed ? darken_mode_ : SDL_BLENDMODE_MUL;
    case Blend::minimum:
        return capabilities_.minimum_composed ? minimum_mode_ : SDL_BLENDMODE_BLEND;
    case Blend::lighten:
        // The multiply mode: the source times what is under it, plus what is
        // under it times one less the source alpha.
        return SDL_BLENDMODE_MUL;
    }
    return SDL_BLENDMODE_NONE;
}

SDL_ScaleMode Executor::scale_mode(Sampling sampling) const noexcept {
    switch (sampling) {
    case Sampling::nearest:
        return SDL_SCALEMODE_NEAREST;
    case Sampling::linear:
        return SDL_SCALEMODE_LINEAR;
    case Sampling::pixel_art:
#if SDL_VERSION_ATLEAST(3, 4, 0)
        if (capabilities_.pixel_art_sampling)
            return SDL_SCALEMODE_PIXELART;
#endif
        return SDL_SCALEMODE_NEAREST;
    }
    return SDL_SCALEMODE_NEAREST;
}

bool Executor::run_draw(
    Run& run, const CardFrame& frame, const Batch& batch, Index first_index, uint32_t index_count
) {
    Target* target = batch.target == TargetHandle{} ? nullptr : find_target(batch.target);
    if (!bind(run, target) || !set_clip(run, batch.scissored, batch.scissor))
        return false;
    // A target drawn into must be halved again before its next resolve.
    if (target != nullptr)
        target->halved_in = 0;
    SDL_Texture* texture = nullptr;
    const SDL_BlendMode mode = blend_mode(batch.blend);
    if (batch.page != PageHandle{}) {
        Level& level = find_page(batch.page)->levels[batch.level];
        texture = level.texture;
        if (!set_texture_blend(texture, level.blend, mode) ||
            !set_texture_sampling(texture, level.scale, scale_mode(batch.sampling)))
            return false;
    } else if (!set_draw_blend(run, mode)) {
        return false;
    }
    constexpr auto stride = static_cast<int>(sizeof(Vertex));
    bool drawn = false;
    if (batch.blend == Blend::darken && !capabilities_.darken_composed) {
        // The multiply mode reads the source colour, so the fallback draws
        // a copy of the batch's vertices with their colours black, which
        // leaves what is under them times one less the alpha.
        scratch_.resize(index_count);
        for (uint32_t at = 0; at < index_count; ++at) {
            Vertex copy = frame.vertices[frame.indices[std::size_t{first_index} + at]];
            copy.colour.red = 0.0F;
            copy.colour.green = 0.0F;
            copy.colour.blue = 0.0F;
            scratch_[at] = copy;
        }
        drawn = SDL_RenderGeometryRaw(
            renderer_,
            texture,
            &scratch_[0].x,
            stride,
            reinterpret_cast<const SDL_FColor*>(&scratch_[0].colour),
            stride,
            &scratch_[0].u,
            stride,
            static_cast<int>(index_count),
            nullptr,
            0,
            0
        );
    } else {
        const Vertex* vertices = frame.vertices.data();
        drawn = SDL_RenderGeometryRaw(
            renderer_,
            texture,
            &vertices[0].x,
            stride,
            reinterpret_cast<const SDL_FColor*>(&vertices[0].colour),
            stride,
            &vertices[0].u,
            stride,
            static_cast<int>(frame.vertices.size()),
            frame.indices.data() + first_index,
            static_cast<int>(index_count),
            static_cast<int>(sizeof(Index))
        );
    }
    if (!drawn)
        return fail("SDL_RenderGeometryRaw");
    ++counts_.draw_calls;
    counts_.triangles += index_count / 3;
    return true;
}

bool Executor::run_clear(Run& run, const Batch& batch) {
    Target* target = batch.target == TargetHandle{} ? nullptr : find_target(batch.target);
    if (!bind(run, target))
        return false;
    // A target cleared must be halved again before its next resolve.
    if (target != nullptr)
        target->halved_in = 0;
    const Colour& colour = batch.colour;
    if (!SDL_SetRenderDrawColorFloat(
            renderer_, colour.red, colour.green, colour.blue, colour.alpha
        ))
        return fail("SDL_SetRenderDrawColorFloat");
    run.colour_changed = true;
    if (!SDL_RenderClear(renderer_))
        return fail("SDL_RenderClear");
    ++counts_.clears;
    return true;
}

bool Executor::halve(Run& run, Target& source, uint32_t count) {
    if (source.halved_in != run_serial_) {
        source.halved_in = run_serial_;
        source.halved_count = 0;
    }
    while (source.halved_count < count) {
        const uint32_t level = source.halved_count;
        SDL_Texture* from = level == 0 ? source.texture : source.halves[level - 1];
        SDL_BlendMode& from_blend = level == 0 ? source.blend : source.half_blends[level - 1];
        if (!bind_texture(run, source.halves[level]) ||
            !set_texture_blend(from, from_blend, SDL_BLENDMODE_NONE))
            return false;
        // Each half is exactly half the size of the texture it is drawn
        // from, so each of its pixels lands on the corner of four of that
        // texture's and the LINEAR draw weighs the four evenly.
        if (!SDL_RenderTexture(renderer_, from, nullptr, nullptr))
            return fail("SDL_RenderTexture halving a render target");
        ++source.halved_count;
        ++counts_.halvings;
    }
    return true;
}

bool Executor::run_resolve(Run& run, const Batch& batch) {
    Target* source = find_target(batch.source);
    SDL_Texture* picture = source->texture;
    SDL_BlendMode* picture_blend = &source->blend;
    // A factor above twice the destination's reduces through its halves to
    // twice that factor; a lesser factor whose target keeps a half for the
    // two-level reduction resolves from the texture itself, since the one
    // LINEAR draw reduces it exactly, and a target at the destination's own
    // factor lands texel for texel.
    Target* destination = batch.target == TargetHandle{} ? nullptr : find_target(batch.target);
    const uint32_t halvings =
        resolve_halvings(source->factor, destination != nullptr ? destination->factor : 1U);
    if (halvings != 0) {
        if (!halve(run, *source, halvings))
            return false;
        picture = source->halves[halvings - 1];
        picture_blend = &source->half_blends[halvings - 1];
    }
    if (!bind(run, destination) || !set_clip(run, batch.scissored, batch.scissor) ||
        !set_texture_blend(picture, *picture_blend, blend_mode(batch.blend)))
        return false;
    // The destination is written, so its own half, if it has one, is stale.
    if (destination != nullptr)
        destination->halved_in = 0;
    // A shift lands the picture between the destination's pixels, which the
    // target's LINEAR sampling draws.
    const SDL_FRect landed{
        static_cast<float>(batch.destination.x) + batch.shift_x,
        static_cast<float>(batch.destination.y) + batch.shift_y,
        static_cast<float>(batch.destination.width),
        static_cast<float>(batch.destination.height)
    };
    if (!SDL_RenderTexture(renderer_, picture, nullptr, &landed))
        return fail("SDL_RenderTexture of a render target");
    ++counts_.resolves;
    return true;
}

bool Executor::run_blend_reduce(Run& run, const Batch& batch) {
    Target* source = find_target(batch.source);
    if (!halve(run, *source, 1))
        return false;
    Target* destination = batch.target == TargetHandle{} ? nullptr : find_target(batch.target);
    if (!bind(run, destination) || !set_clip(run, batch.scissored, batch.scissor))
        return false;
    // The destination is written, so its own half, if it has one, is stale.
    if (destination != nullptr)
        destination->halved_in = 0;
    const Rect& part = batch.source_part;
    const SDL_FRect texture_part{
        static_cast<float>(part.x),
        static_cast<float>(part.y),
        static_cast<float>(part.width),
        static_cast<float>(part.height)
    };
    const SDL_FRect half_part{
        static_cast<float>(part.x / 2),
        static_cast<float>(part.y / 2),
        static_cast<float>(part.width / 2),
        static_cast<float>(part.height / 2)
    };
    // A shift lands the picture between the destination's pixels, which the
    // target's LINEAR sampling draws.
    const SDL_FRect landed{
        static_cast<float>(batch.destination.x) + batch.shift_x,
        static_cast<float>(batch.destination.y) + batch.shift_y,
        static_cast<float>(batch.destination.width),
        static_cast<float>(batch.destination.height)
    };
    // The half at twice the scale, which at a scale of one half is the
    // whole result: a box of four texture pixels a destination pixel.
    if (!set_texture_blend(source->halves[0], source->half_blends[0], SDL_BLENDMODE_NONE))
        return false;
    if (!SDL_RenderTexture(renderer_, source->halves[0], &half_part, &landed))
        return fail("SDL_RenderTexture of a render target's half");
    // Then the part itself over it at alpha 1 - t, t = log2(1 / scale),
    // which at a scale of 1 replaces the half entirely.
    const double scale = static_cast<double>(batch.destination.width) / part.width;
    const double t = std::clamp(std::log2(1.0 / scale), 0.0, 1.0);
    if (t < 1.0) {
        if (!set_texture_blend(source->texture, source->blend, SDL_BLENDMODE_BLEND))
            return false;
        if (!SDL_SetTextureAlphaModFloat(source->texture, static_cast<float>(1.0 - t)))
            return fail("SDL_SetTextureAlphaModFloat of a render target");
        const bool drawn = SDL_RenderTexture(renderer_, source->texture, &texture_part, &landed);
        // The target's alpha is put back whatever the draw did, so that a
        // later resolve or halving reads it whole.
        const bool put_back = SDL_SetTextureAlphaModFloat(source->texture, 1.0F);
        if (!drawn)
            return fail("SDL_RenderTexture of a render target over its half");
        if (!put_back)
            return fail("SDL_SetTextureAlphaModFloat putting back a render target's alpha");
    }
    ++counts_.blend_reductions;
    return true;
}

bool Executor::restore(Run& run) {
    if (!bind(run, nullptr))
        return false;
    if (run.final_clip_changed &&
        !SDL_SetRenderClipRect(renderer_, run.saved_clipped ? &run.saved_clip : nullptr))
        return fail("SDL_SetRenderClipRect putting back the final target's scissor");
    run.final_clip_changed = false;
    if (run.colour_changed && !SDL_SetRenderDrawColorFloat(
                                  renderer_,
                                  run.saved_colour.r,
                                  run.saved_colour.g,
                                  run.saved_colour.b,
                                  run.saved_colour.a
                              ))
        return fail("SDL_SetRenderDrawColorFloat putting back the draw colour");
    run.colour_changed = false;
    if (run.draw_blend_changed && !SDL_SetRenderDrawBlendMode(renderer_, run.saved_draw_blend))
        return fail("SDL_SetRenderDrawBlendMode putting back the draw blend mode");
    run.draw_blend_changed = false;
    return true;
}

bool Executor::execute(const CardFrame& frame, SDL_Texture* final_target) {
    const bool ran = run_frame(frame, final_target);
    for (const PageHandle page : retired_pages_)
        destroy_page(page);
    retired_pages_.clear();
    return ran;
}

bool Executor::run_frame(const CardFrame& frame, SDL_Texture* final_target) {
    frame_refused_ = false;
    if (!is_open())
        return refuse("the executor is not open");
    std::string fault = check_frame(frame);
    if (fault.empty())
        fault = check_handles(frame, final_target);
    if (!fault.empty()) {
        ++counts_.frames_refused;
        frame_refused_ = true;
        return refuse("frame refused: " + fault);
    }
    ++run_serial_;
    Run run;
    run.final_target = final_target;
    if (!SDL_SetRenderTarget(renderer_, final_target))
        return fail("SDL_SetRenderTarget of the final target");
    run.bound = final_target;
    run.final_bound = true;
    ++counts_.target_switches;
    run.saved_clipped = SDL_RenderClipEnabled(renderer_);
    if (!SDL_GetRenderClipRect(renderer_, &run.saved_clip) ||
        !SDL_GetRenderDrawColorFloat(
            renderer_,
            &run.saved_colour.r,
            &run.saved_colour.g,
            &run.saved_colour.b,
            &run.saved_colour.a
        ) ||
        !SDL_GetRenderDrawBlendMode(renderer_, &run.saved_draw_blend))
        return fail("SDL reading the final target's state");
    run.final_clip = {
        true,
        run.saved_clipped,
        {run.saved_clip.x, run.saved_clip.y, run.saved_clip.w, run.saved_clip.h}
    };
    run.draw_blend = run.saved_draw_blend;
    const std::size_t count = frame.batches.size();
    for (std::size_t position = 0; position < count; ++position) {
        const Batch& batch = frame.batches[position];
        bool ran = true;
        switch (batch.operation) {
        case Operation::draw: {
            const Index first_index = batch.first_index;
            uint32_t index_count = batch.index_count;
            while (position + 1 < count &&
                   mergeable(batch, frame.batches[position + 1], first_index + index_count)) {
                index_count += frame.batches[position + 1].index_count;
                ++position;
                ++counts_.batches_merged;
            }
            if (index_count != 0)
                ran = run_draw(run, frame, batch, first_index, index_count);
            break;
        }
        case Operation::clear:
            ran = run_clear(run, batch);
            break;
        case Operation::resolve:
            ran = run_resolve(run, batch);
            break;
        case Operation::blend_reduce:
            ran = run_blend_reduce(run, batch);
            break;
        }
        if (!ran) {
            // The first failure is the one reported; putting things back is
            // best effort.
            const std::string first_error = error_;
            std::ignore = restore(run);
            error_ = first_error;
            return false;
        }
    }
    if (!restore(run))
        return false;
    ++counts_.frames;
    return true;
}

} // namespace oa::app::card
