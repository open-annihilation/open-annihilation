// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Full tier's sprite stage (runtime_full.hpp): the frame's sprites,
// particle squares, lines and selection lines as quads of the card's
// command list, in the planner's order, each sprite from its cell on the
// sprite pages; a draw at a time, so that the branch keeps the list's order
// between the sprites and the models.
#include "runtime_full.hpp"

#include "oa/present/world_renderer/world_fog.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace oa::app::full {

namespace {

namespace gpu = oa::present::gpu_world;
using oa::formats::gaf::RenderedFrame;

/// The bit that tells a key made from a GAF frame, one decoded for this
/// frame's draws alone, from one made from a rendered frame the match
/// keeps, so that the two kinds of address never name one cell.
constexpr uint64_t decoded_key_bit = uint64_t{1} << 63;

/// The vertex colour and alpha of a sprite the planner blends through the
/// alpha table: one half, which under the premultiplied blend halves the
/// sprite and leaves half of what is under it.
constexpr float translucent_level = 0.5F;

/// Highest value of a colour's channel.
constexpr float channel_full = 255.0F;

/// The vertex colour of an explosion's flash at FlashStrength::reduced:
/// one half, which halves the light of each texel, as the processor halves
/// the light table's row.
constexpr float reduced_flash_light = 0.5F;

/// The GAF frame each rendered frame of a list was decoded from this
/// frame, for the frames decoded anew each frame (WorldDrawList::decoded).
using DecodedFrom = std::unordered_map<const RenderedFrame*, const oa::formats::gaf::Frame*>;

/// Returns the key a sprite's picture is held under on the pages: the
/// address of the GAF frame it was decoded from, for a frame the list
/// decoded for this frame's draws alone, which lives in the match's
/// archives; else the address of the rendered frame itself, which the
/// match keeps for a feature's animation.
///
/// @param frame the rendered frame drawn
/// @param decoded_from the frames decoded this frame, by their rendered frame
/// @return the key
uint64_t frame_key(const RenderedFrame* frame, const DecodedFrom& decoded_from) {
    if (const auto found = decoded_from.find(frame); found != decoded_from.end())
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(found->second)) | decoded_key_bit;
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(frame));
}

/// What tells one batch of the stage from the next.
struct BatchKey {
    card::PageHandle page{};
    card::Blend blend{card::Blend::none};
    card::Sampling sampling{card::Sampling::nearest};
    /// The batch draws a feature's frames, cut off where the map ends.
    bool on_map{};

    friend bool operator==(const BatchKey&, const BatchKey&) = default;
};

/// Appends the stage's quads to a frame, consecutive quads that share
/// their key in one batch.
class Emitter {
  public:

    /// Readies the emitter for a view.
    ///
    /// @param frame the frame appended to
    /// @param view the view every batch draws through
    /// @param list the frame's draws, whose shown map a feature's frames are
    ///     cut off at; null for none
    Emitter(card::CardFrame& frame, const SceneView& view, const WorldDrawList* list)
        : frame_(frame), view_(view) {
        scissor_.x = static_cast<int32_t>(std::lround(view.origin_x));
        scissor_.y = static_cast<int32_t>(std::lround(view.origin_y));
        scissor_.width =
            static_cast<int32_t>(std::lround(static_cast<double>(view.width) * view.scale));
        scissor_.height =
            static_cast<int32_t>(std::lround(static_cast<double>(view.height) * view.scale));
        if (list != nullptr)
            map_scissor_ = shown_map_scissor(
                view,
                view.origin_x,
                view.origin_y,
                list->shown_map_width,
                list->shown_map_height,
                scissor_
            );
    }

    /// Tells whether a feature's frames show anywhere: whether the map the
    /// view shows lies on the battlefield.
    ///
    /// @return false when the map lies wholly off it
    [[nodiscard]] bool map_shown() const noexcept {
        return map_scissor_.width > 0 && map_scissor_.height > 0;
    }

    /// Returns a scene pixel column's place in the target.
    ///
    /// @param x scene pixel column, at the zoom
    /// @return the target pixel column
    [[nodiscard]] float place_x(float x) const noexcept {
        return view_.origin_x + (x - static_cast<float>(view_.offset.x) * view_.zoom) * view_.scale;
    }

    /// Returns a scene pixel row's place in the target.
    ///
    /// @param y scene pixel row, at the zoom
    /// @return the target pixel row
    [[nodiscard]] float place_y(float y) const noexcept {
        return view_.origin_y + (y - static_cast<float>(view_.offset.y) * view_.zoom) * view_.scale;
    }

    /// Appends an axis-aligned quad, in target pixels, to the batch of a
    /// key, opening one when the last batch has another key.
    ///
    /// @param key the batch's page, blend and sampling
    /// @param x the left edge
    /// @param y the top edge
    /// @param width pixels across
    /// @param height pixels down
    /// @param u0 the left edge's texture coordinate
    /// @param v0 the top edge's
    /// @param u1 the right edge's
    /// @param v1 the bottom edge's
    /// @param colour the colour of every corner
    void quad(
        const BatchKey& key,
        float x,
        float y,
        float width,
        float height,
        float u0,
        float v0,
        float u1,
        float v1,
        const card::Colour& colour
    ) {
        open(key);
        card::append_quad(frame_, x, y, width, height, u0, v0, u1, v1, colour);
        frame_.batches[last_].index_count += indices_per_quad;
    }

    /// Appends a line's quad (append_line_quad) to the untextured batch.
    ///
    /// @param x0 the first pixel's centre column, in target pixels
    /// @param y0 its centre row
    /// @param x1 the last pixel's centre column
    /// @param y1 its centre row
    /// @param width pixels across the line
    /// @param colour the line's colour
    void line(float x0, float y0, float x1, float y1, float width, const card::Colour& colour) {
        open(BatchKey{});
        append_line_quad(frame_, x0, y0, x1, y1, width, colour);
        frame_.batches[last_].index_count += indices_per_quad;
    }

    /// Returns the batches opened.
    ///
    /// @return the count
    [[nodiscard]] uint32_t batches() const noexcept { return batches_; }

    /// Returns the batches the emitter opened, in the order it opened them.
    ///
    /// @return their positions among the frame's batches
    [[nodiscard]] const std::vector<std::size_t>& opened() const noexcept { return opened_; }

  private:

    /// Indices a quad adds: two triangles.
    static constexpr uint32_t indices_per_quad = 6;

    /// Opens a batch of a key unless the last batch opened has it, is still
    /// the frame's last and holds the indices up to the frame's end. Another
    /// stage drawing to the same frame between two of this stage's draws
    /// appends batches of its own, which a quad of this stage must not
    /// join: it would draw from that stage's page with its blend.
    ///
    /// @param key the batch's page, blend and sampling
    void open(const BatchKey& key) {
        if (open_ && key_ == key && last_ + 1 == frame_.batches.size()) {
            const card::Batch& last = frame_.batches[last_];
            if (std::size_t{last.first_index} + last.index_count == frame_.indices.size())
                return;
        }
        card::Batch batch;
        batch.operation = card::Operation::draw;
        batch.target = view_.target;
        batch.page = key.page;
        batch.level = 0;
        batch.blend = key.blend;
        batch.sampling = key.sampling;
        batch.scissored = true;
        batch.scissor = key.on_map ? map_scissor_ : scissor_;
        batch.first_index = static_cast<card::Index>(frame_.indices.size());
        batch.index_count = 0;
        frame_.batches.push_back(batch);
        open_ = true;
        key_ = key;
        last_ = frame_.batches.size() - 1;
        opened_.push_back(last_);
        ++batches_;
    }

    card::CardFrame& frame_;
    const SceneView& view_;
    card::Rect scissor_{};
    card::Rect map_scissor_{}; ///< the part of scissor_ over the map the view shows
    bool open_{};
    BatchKey key_{};
    std::size_t last_{};                ///< the position of the batch last opened
    std::vector<std::size_t> opened_{}; ///< the positions of every batch opened
    uint32_t batches_{};
};

/// A sprite placed on the pages this frame, to be found again after the
/// frame's draws are emitted when the pages evicted frames meanwhile.
struct Placed {
    uint64_t key{};
    gpu::DrawMode mode{};
    gpu::FrameRecord record{};
};

} // namespace

CellFog cell_fog(const SightView& sight, int32_t map_x, int32_t map_z) noexcept {
    if (sight.width <= 0 || sight.height <= 0 || map_x < 0 || map_z < 0)
        return CellFog::seen;
    const int32_t cell_x = map_x / oa::present::world_renderer::fog_cell_pixels;
    const int32_t cell_z = map_z / oa::present::world_renderer::fog_cell_pixels;
    if (cell_x >= sight.width || cell_z >= sight.height)
        return CellFog::seen;
    const auto index = static_cast<std::size_t>(cell_z) * static_cast<std::size_t>(sight.width) +
                       static_cast<std::size_t>(cell_x);
    const bool in_sight = index < sight.coverage.size() && sight.coverage[index] != 0;
    if (!in_sight && sight.line_of_sight)
        return CellFog::unseen;
    const bool mapped = !sight.mapping || (index < sight.player_bits.size() &&
                                           (sight.player_bits[index] & sight.viewer_bit) != 0);
    return mapped ? CellFog::seen : CellFog::unmapped;
}

bool sprite_kind(WorldDrawKind kind) noexcept {
    switch (kind) {
    case WorldDrawKind::sprite:
    case WorldDrawKind::blended_sprite:
    case WorldDrawKind::lit_sprite:
    case WorldDrawKind::pixel_square:
    case WorldDrawKind::line:
    case WorldDrawKind::selection_line:
        return true;
    case WorldDrawKind::commit:
    case WorldDrawKind::commit_always:
    case WorldDrawKind::model:
    case WorldDrawKind::projectile:
    case WorldDrawKind::debris:
    case WorldDrawKind::fragment:
    case WorldDrawKind::lens:
        break;
    }
    return false;
}

bool model_kind(WorldDrawKind kind) noexcept {
    switch (kind) {
    case WorldDrawKind::model:
    case WorldDrawKind::projectile:
    case WorldDrawKind::debris:
    case WorldDrawKind::fragment:
        return true;
    case WorldDrawKind::commit:
    case WorldDrawKind::commit_always:
    case WorldDrawKind::sprite:
    case WorldDrawKind::blended_sprite:
    case WorldDrawKind::lit_sprite:
    case WorldDrawKind::pixel_square:
    case WorldDrawKind::line:
    case WorldDrawKind::selection_line:
    case WorldDrawKind::lens:
        break;
    }
    return false;
}

card::Sampling sprite_sampling(float zoom) noexcept {
    if (std::floor(zoom) == zoom)
        return card::Sampling::nearest;
    return zoom < 1.0F ? card::Sampling::linear : card::Sampling::pixel_art;
}

float line_width(const SceneView& view) noexcept {
    return std::max(1.0F, view.zoom) * view.scale;
}

card::Colour
flat_colour(const std::array<uint8_t, 3>& rgb, const std::array<uint8_t, 256>* gamma) noexcept {
    const auto shown = [&](uint8_t channel) {
        return static_cast<float>(gamma != nullptr ? (*gamma)[channel] : channel) / channel_full;
    };
    return {shown(rgb[0]), shown(rgb[1]), shown(rgb[2]), 1.0F};
}

void append_line_quad(
    card::CardFrame& frame,
    float x0,
    float y0,
    float x1,
    float y1,
    float width,
    const card::Colour& colour
) {
    float along_x = x1 - x0;
    float along_y = y1 - y0;
    const float length = std::sqrt(along_x * along_x + along_y * along_y);
    if (length > 0.0F) {
        along_x /= length;
        along_y /= length;
    } else {
        // A line of one pixel runs along its row.
        along_x = 1.0F;
        along_y = 0.0F;
    }
    const float half = width * 0.5F;
    const float start_x = x0 - along_x * half;
    const float start_y = y0 - along_y * half;
    const float end_x = x1 + along_x * half;
    const float end_y = y1 + along_y * half;
    const float across_x = -along_y * half;
    const float across_y = along_x * half;
    const auto first = static_cast<card::Index>(frame.vertices.size());
    frame.vertices.push_back({start_x + across_x, start_y + across_y, colour, 0.0F, 0.0F});
    frame.vertices.push_back({end_x + across_x, end_y + across_y, colour, 0.0F, 0.0F});
    frame.vertices.push_back({end_x - across_x, end_y - across_y, colour, 0.0F, 0.0F});
    frame.vertices.push_back({start_x - across_x, start_y - across_y, colour, 0.0F, 0.0F});
    frame.indices.push_back(first);
    frame.indices.push_back(first + 1);
    frame.indices.push_back(first + 2);
    frame.indices.push_back(first);
    frame.indices.push_back(first + 2);
    frame.indices.push_back(first + 3);
}

/// One frame's emission: what emit_sprites kept on its stack, so that the
/// branch can hand the list's draws over one at a time.
struct SpriteFrame::Impl {
    const SpriteStageInputs& inputs;
    gpu::SpritePages& pages;
    const SpritePageHooks& hooks;
    card::CardFrame& frame;
    SpriteStageResult result{};
    bool drawing{}; ///< the inputs name a list and a view that draws
    // Where the frame stood before the stage, to take the stage back.
    std::size_t vertices_before{};
    std::size_t indices_before{};
    std::size_t batches_before{};
    uint64_t evictions_before{};
    DecodedFrom decoded_from{};
    std::vector<Placed> placed{};
    Emitter emitter;
    card::Sampling sampling{card::Sampling::nearest};
    float zoom{1.0F};
    float width{1.0F}; ///< a line's width in target pixels
    bool greys{};      ///< the pages hold a gray table

    /// Readies the frame's emission.
    ///
    /// @param stage_inputs the list, the view, the sight, the palette and the gamma
    /// @param sprite_pages the sprite pages
    /// @param page_hooks the host's pages
    /// @param card_frame the card's frame
    Impl(
        const SpriteStageInputs& stage_inputs,
        gpu::SpritePages& sprite_pages,
        const SpritePageHooks& page_hooks,
        card::CardFrame& card_frame
    )
        : inputs(stage_inputs), pages(sprite_pages), hooks(page_hooks), frame(card_frame),
          emitter(card_frame, stage_inputs.view, stage_inputs.list) {
        drawing = inputs.list != nullptr && inputs.view.width > 0 && inputs.view.height > 0;
        if (!drawing)
            return;
        vertices_before = frame.vertices.size();
        indices_before = frame.indices.size();
        batches_before = frame.batches.size();
        evictions_before = pages.statistics().evictions;
        for (const auto& [source, decoded] : inputs.list->decoded_of)
            if (decoded != nullptr)
                decoded_from.emplace(decoded, source);
        placed.reserve(inputs.list->sprites.size());
        sampling = sprite_sampling(inputs.view.zoom);
        zoom = inputs.view.zoom;
        width = line_width(inputs.view);
        greys = pages.has_gray_table();
    }

    /// Emits a sprite's quad from its cell on the pages.
    ///
    /// @param sprite the sprite
    /// @param blended the planner blends it through the alpha table
    void emit_sprite(const SpriteDraw& sprite, bool blended) {
        // A frame drawn too far out for shadows draws no feature's shadow,
        // and a feature's frames show only over the map.
        if ((sprite.shadow && !shadows_drawn(*inputs.list)) ||
            (sprite.on_map && !emitter.map_shown()))
            return;
        if (sprite.frame == nullptr || hooks.card_page == nullptr) {
            ++result.refused;
            return;
        }
        const SceneView& view = inputs.view;
        // The fog's state where the sprite is drawn: the map pixel under
        // its place, as the fog lays its tiles by map column and row alone,
        // whatever height the point was lifted by.
        const CellFog fog =
            cell_fog(inputs.sight, view.camera_x + sprite.place.x, view.camera_y + sprite.place.y);
        const bool greyed = fog == CellFog::unseen && greys && !inputs.sight.dithered;
        const auto mode = greyed ? gpu::DrawMode::greyed : gpu::DrawMode::opaque;
        const uint64_t key = frame_key(sprite.frame, decoded_from);
        const gpu::FrameResult found = pages.frame(key, mode, *sprite.frame);
        if (found.status != gpu::FrameStatus::ok) {
            ++result.refused;
            return;
        }
        const card::PageHandle page = hooks.card_page(hooks.context, found.record.page);
        if (page == card::PageHandle{}) {
            ++result.refused;
            return;
        }
        placed.push_back({key, mode, found.record});
        const auto size = static_cast<float>(pages.pages()[found.record.page].size);
        const gpu::TexelRect& rect = found.record.rect;
        // The frame lies over the map pixels from its place less its
        // origin, where the terrain draws them at the zoom.
        const float left = static_cast<float>(sprite.place.x - sprite.frame->origin_x) * zoom;
        const float top = static_cast<float>(sprite.place.y - sprite.frame->origin_y) * zoom;
        // A feature's shadow frame draws as dark as the list's shadow
        // level: under the premultiplied blend, a vertex colour and alpha of
        // the level's strength mix that much of the frame (or of its blend)
        // into what is under it.
        const float strength =
            sprite.shadow ? oa::present::model::shadow_level_strength(inputs.list->shadow_level)
                          : 1.0F;
        const card::Colour opaque{strength, strength, strength, strength};
        const float level = translucent_level * strength;
        const card::Colour translucent{level, level, level, level};
        emitter.quad(
            {page, card::Blend::alpha_premultiplied, sampling, sprite.on_map},
            emitter.place_x(left),
            emitter.place_y(top),
            static_cast<float>(sprite.frame->width) * zoom * view.scale,
            static_cast<float>(sprite.frame->height) * zoom * view.scale,
            static_cast<float>(rect.x) / size,
            static_cast<float>(rect.y) / size,
            static_cast<float>(rect.x + rect.width) / size,
            static_cast<float>(rect.y + rect.height) / size,
            blended ? translucent : opaque
        );
        ++result.sprites;
        if (greyed)
            ++result.greyed;
    }

    /// Emits an explosion's flash: a quad from its frame's lit cell on the
    /// pages, which the lighten blend makes light what is under it, the
    /// vertex colour halving each texel's light at FlashStrength::reduced
    /// and the vertex alpha 0, so that the texels' own alpha of 0 is kept.
    ///
    /// @param sprite the flash
    void emit_lit(const SpriteDraw& sprite) {
        if (sprite.frame == nullptr || hooks.card_page == nullptr) {
            ++result.refused;
            return;
        }
        const uint64_t key = frame_key(sprite.frame, decoded_from);
        const gpu::FrameResult found = pages.frame(key, gpu::DrawMode::lit, *sprite.frame);
        if (found.status != gpu::FrameStatus::ok) {
            ++result.refused;
            return;
        }
        const card::PageHandle page = hooks.card_page(hooks.context, found.record.page);
        if (page == card::PageHandle{}) {
            ++result.refused;
            return;
        }
        placed.push_back({key, gpu::DrawMode::lit, found.record});
        const SceneView& view = inputs.view;
        const auto size = static_cast<float>(pages.pages()[found.record.page].size);
        const gpu::TexelRect& rect = found.record.rect;
        // The frame lies over the map pixels from its place less its
        // origin, where the terrain draws them at the zoom.
        const float left = static_cast<float>(sprite.place.x - sprite.frame->origin_x) * zoom;
        const float top = static_cast<float>(sprite.place.y - sprite.frame->origin_y) * zoom;
        const float light =
            inputs.list->flash_strength == FlashStrength::reduced ? reduced_flash_light : 1.0F;
        emitter.quad(
            {page, card::Blend::lighten, sampling},
            emitter.place_x(left),
            emitter.place_y(top),
            static_cast<float>(sprite.frame->width) * zoom * view.scale,
            static_cast<float>(sprite.frame->height) * zoom * view.scale,
            static_cast<float>(rect.x) / size,
            static_cast<float>(rect.y) / size,
            static_cast<float>(rect.x + rect.width) / size,
            static_cast<float>(rect.y + rect.height) / size,
            {light, light, light, 0.0F}
        );
        ++result.sprites;
        ++result.lit;
    }

    /// Emits one draw of the list.
    ///
    /// @param draw the draw
    void emit(const WorldDraw& draw) {
        if (!drawing)
            return;
        const WorldDrawList& list = *inputs.list;
        const SceneView& view = inputs.view;
        switch (draw.kind) {
        case WorldDrawKind::sprite:
        case WorldDrawKind::blended_sprite:
            if (draw.index < list.sprites.size())
                emit_sprite(list.sprites[draw.index], draw.kind == WorldDrawKind::blended_sprite);
            break;
        case WorldDrawKind::lit_sprite:
            if (draw.index < list.sprites.size())
                emit_lit(list.sprites[draw.index]);
            break;
        case WorldDrawKind::pixel_square: {
            if (draw.index >= list.squares.size())
                break;
            const SquareDraw& square = list.squares[draw.index];
            if (square.right <= square.left || square.bottom <= square.top)
                break;
            emitter.quad(
                BatchKey{},
                emitter.place_x(static_cast<float>(square.left)),
                emitter.place_y(static_cast<float>(square.top)),
                static_cast<float>(square.right - square.left) * view.scale,
                static_cast<float>(square.bottom - square.top) * view.scale,
                0.0F,
                0.0F,
                0.0F,
                0.0F,
                flat_colour(square.color, inputs.gamma)
            );
            ++result.squares;
            break;
        }
        case WorldDrawKind::line: {
            if (draw.index >= list.lines.size())
                break;
            const LineDraw& line = list.lines[draw.index];
            emitter.line(
                emitter.place_x(static_cast<float>(line.x0) + 0.5F),
                emitter.place_y(static_cast<float>(line.y0) + 0.5F),
                emitter.place_x(static_cast<float>(line.x1) + 0.5F),
                emitter.place_y(static_cast<float>(line.y1) + 0.5F),
                width,
                flat_colour(line.color, inputs.gamma)
            );
            ++result.lines;
            break;
        }
        case WorldDrawKind::selection_line: {
            if (draw.index >= list.lines.size() || inputs.palette == nullptr)
                break;
            // The line's pixels are map pixels about the camera, which the
            // bridge writes back at the zoom.
            const LineDraw& line = list.lines[draw.index];
            const auto entry = static_cast<std::size_t>(line.palette_index) * 4U;
            const std::array<uint8_t, 3> rgb{
                (*inputs.palette)[entry], (*inputs.palette)[entry + 1], (*inputs.palette)[entry + 2]
            };
            emitter.line(
                emitter.place_x((static_cast<float>(line.x0) + 0.5F) * zoom),
                emitter.place_y((static_cast<float>(line.y0) + 0.5F) * zoom),
                emitter.place_x((static_cast<float>(line.x1) + 0.5F) * zoom),
                emitter.place_y((static_cast<float>(line.y1) + 0.5F) * zoom),
                width,
                flat_colour(rgb, inputs.gamma)
            );
            ++result.lines;
            break;
        }
        case WorldDrawKind::commit:
        case WorldDrawKind::commit_always:
        case WorldDrawKind::model:
        case WorldDrawKind::projectile:
        case WorldDrawKind::debris:
        case WorldDrawKind::fragment:
        case WorldDrawKind::lens:
            break;
        }
    }

    /// Ends the emission (SpriteFrame::finish).
    ///
    /// @return what was emitted
    SpriteStageResult finish() {
        if (!drawing)
            return result;
        result.batches = emitter.batches();
        // The pages evict least recently used first, so a frame placed this
        // frame goes only once every older one has: when the frame's
        // distinct sprites exceed the pages' memory. A cell drawn from after
        // its frame left it would show another sprite, so the stage then
        // draws nothing.
        if (pages.statistics().evictions != evictions_before)
            for (const Placed& sprite : placed) {
                const gpu::FrameResult held = pages.find(sprite.key, sprite.mode);
                if (held.status == gpu::FrameStatus::ok && held.record.page == sprite.record.page &&
                    held.record.rect.x == sprite.record.rect.x &&
                    held.record.rect.y == sprite.record.rect.y)
                    continue;
                take_back();
                result = {};
                result.pages_overflowed = true;
                break;
            }
        return result;
    }

    /// Takes the stage's draws out of the frame: where its batches are the
    /// frame's last ones, by cutting the frame back to where it stood when
    /// the stage began; where another stage's batches lie among them, by
    /// emptying each of the stage's batches, whose vertices then stay in
    /// the frame but draw nothing. An emptied batch names no page, since
    /// the page it named may be gone by the time the frame runs.
    void take_back() {
        const std::vector<std::size_t>& opened = emitter.opened();
        const bool last = opened.size() == frame.batches.size() - batches_before;
        if (last) {
            frame.vertices.resize(vertices_before);
            frame.indices.resize(indices_before);
            frame.batches.resize(batches_before);
            return;
        }
        for (const std::size_t position : opened) {
            frame.batches[position].index_count = 0;
            frame.batches[position].page = {};
        }
    }
};

SpriteFrame::SpriteFrame(
    const SpriteStageInputs& inputs,
    gpu::SpritePages& pages,
    const SpritePageHooks& hooks,
    card::CardFrame& frame
)
    : impl_(std::make_unique<Impl>(inputs, pages, hooks, frame)) {
}

SpriteFrame::~SpriteFrame() = default;

void SpriteFrame::emit(const WorldDraw& draw) {
    impl_->emit(draw);
}

SpriteStageResult SpriteFrame::finish() {
    return impl_->finish();
}

SpriteStageResult emit_sprites(
    const SpriteStageInputs& inputs,
    gpu::SpritePages& pages,
    const SpritePageHooks& hooks,
    card::CardFrame& frame
) {
    SpriteFrame emission(inputs, pages, hooks, frame);
    if (inputs.list != nullptr)
        for (const WorldDraw& draw : inputs.list->draws)
            emission.emit(draw);
    return emission.finish();
}

} // namespace oa::app::full
