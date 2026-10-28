// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Unit, feature, debris and projectile model drawing.
#include "oa/present/model/model_draw.hpp"
#include "oa/base/game_math.hpp"

#include "oa/present/model/mesh_raster.hpp"
#include "oa/present/blit.hpp"
#include "oa/present/display.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/present/polygon.hpp"
#include "oa/present/rle.hpp"
#include "oa/present/span_sample.hpp"
#include "oa/base/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace oa::present::model {
using base::game_math::truncate_low32;

namespace {

using formats::objects3d::FixedVector3;
using sim::model_runtime::PieceFlag;
using sim::model_runtime::PieceState;

constexpr uint16_t piece_visible = static_cast<uint16_t>(PieceFlag::visible);
constexpr uint16_t piece_cached = static_cast<uint16_t>(PieceFlag::cached);
constexpr uint16_t piece_shaded = static_cast<uint16_t>(PieceFlag::shaded);
// Margin added around measured model extents.
constexpr int32_t bounds_margin = 2;
// Build-effect colour ramp: palette 0xa0..0xaf, up then down.
constexpr uint8_t nano_ramp_low = 0xa0;
constexpr uint8_t nano_ramp_high = 0xaf;
// A wave's step that turns its colour back up the ramp, and its place on it.
constexpr uint32_t nano_ramp_turn = 0x10;
constexpr uint32_t nano_ramp_mask = 0xf;
// The build effect's waves: steps every nano_wave_period ticks, and the
// value each unit's id is mixed with to start them.
constexpr uint32_t nano_wave_first_steps = 0x21;
constexpr uint32_t nano_wave_second_steps = 0x39;
constexpr uint32_t nano_wave_period = 0x1e;
constexpr uint32_t nano_wave_first_salt = 5;
constexpr uint32_t nano_wave_second_salt = 9;
// The build effect's clock counts parts of a tick in 1/65536.
constexpr uint32_t pulse_fraction_bits = 16;
constexpr uint32_t pulse_fraction_one = 1U << pulse_fraction_bits;
constexpr float progress_scale = 255.0F;
constexpr int64_t composite_max_pixels = 16 * 1024 * 1024;
constexpr double light_input_scale = 0.01;
// Largest image the arena allocators hand out.
constexpr int64_t image_max_pixels = 16 * 1024 * 1024;

int32_t hi(int32_t value) noexcept {
    return static_cast<int16_t>(static_cast<uint32_t>(value) >> 16);
}

// The whole part of a 16.16 value counted in samples, `samples` to a game
// pixel, rounded down: at one sample, its high word.
int32_t hi_at(int32_t value, uint32_t samples) noexcept {
    return static_cast<int32_t>((static_cast<int64_t>(value) * samples) >> 16);
}

// A margin of bounds_margin game pixels, in samples.
int32_t margin_at(uint32_t samples) noexcept {
    return bounds_margin * static_cast<int32_t>(samples);
}

int32_t wrap_add(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

int32_t wrap_sub(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

uint32_t def_flags(const ModelRef& model) noexcept {
    return model.def != nullptr ? model.def->flags : 0;
}

bool is_digger(const ModelRef& model) noexcept {
    return (def_flags(model) & OA_UNIT_DEF_FLAG_DIGGER) != 0;
}

bool is_building(const Unit& unit) noexcept {
    return (unit.flags & OA_UNIT_FLAG_BUILDING) != 0;
}

int32_t vertex_depth_base(const ModelRef& model) noexcept {
    return model_depth_base + (is_digger(model) ? digger_depth_bias : 0);
}

bool has_image(const ModelState& state) noexcept {
    return state.image.sprite.data != nullptr;
}

uint32_t first_primitive(const PreparedObject& prepared) noexcept {
    return prepared.skips_first ? 1U : 0U;
}

const formats::objects3d::Primitive&
source_of(const formats::objects3d::Object& object, const PreparedPrimitive& primitive) {
    return object.primitives[primitive.source_index];
}

// Frame of an animated primitive's running cursor, ignoring team colouring.
const Sprite* cursor_texture(const PreparedPrimitive& primitive) noexcept {
    if ((primitive.flags & primitive_animated) == 0)
        return primitive.frame != nullptr && primitive.frame->data != nullptr ? primitive.frame
                                                                              : nullptr;
    if (primitive.texture == nullptr || primitive.cursor.sequence == nullptr ||
        primitive.cursor.frame_index >= primitive.texture->frames.size())
        return nullptr;
    const Sprite* sprite = &primitive.texture->frames[primitive.cursor.frame_index];
    return sprite->data != nullptr ? sprite : nullptr;
}

// Gathers a primitive's corners from projected vertices; false when an
// index lies outside the table.
template <typename Vertex>
bool gather(
    const formats::objects3d::Primitive& primitive,
    const std::vector<Vertex>& table,
    std::vector<Vertex>& out
) {
    out.clear();
    for (const uint16_t index : primitive.vertex_indices) {
        if (index >= table.size())
            return false;
        out.push_back(table[index]);
    }
    return true;
}

bool piece_passes(const PieceState& piece, int32_t pass, const Unit& unit) noexcept {
    if ((piece.flags & piece_visible) == 0)
        return false;
    return pass == pass_all_pieces || pass == ((piece.flags >> 1) & 1) ||
           unit.build_remaining != 0.0F;
}

// One side, in samples, of the composite that limits cutting a larger model
// keep: the limits' width or height in game pixels times the samples a game
// pixel is drawn at, so that a unit drawn finer for enhanced anti-aliasing
// is cut where the game's pixels cut it.
int32_t clamped_side(int32_t side, uint32_t samples) noexcept {
    return static_cast<int32_t>(std::min<int64_t>(
        int64_t{std::max(side, int32_t{0})} * samples, std::numeric_limits<uint16_t>::max()
    ));
}

// The samples of the composite that limits cutting a larger model keep, at
// the renderer's samples.
int64_t clamped_plane(const ModelRenderer& renderer) noexcept {
    return int64_t{clamped_side(renderer.composite_limits.width, renderer.samples)} *
           clamped_side(renderer.composite_limits.height, renderer.samples);
}

// The composite grows to fit a model larger than its start size. Under limits
// that cut such a model it grows only as far as those limits at the draw's
// samples, so at the game's pixels not past its start size.
void ensure_composite(ModelRenderer& renderer, int64_t pixels) {
    if (renderer.composite_limits.clamp_oversize)
        pixels = std::min(pixels, clamped_plane(renderer));
    auto& composite = renderer.composite;
    const auto plane = static_cast<int64_t>(composite.pixels.size() / 2);
    if (pixels <= plane || pixels > composite_max_pixels)
        return;
    std::vector<uint8_t> grown(static_cast<std::size_t>(pixels) * 2, 0);
    composite.pixels.swap(grown);
    composite.sprite.data = composite.pixels.data();
    composite.sprite.aux = composite.pixels.data() + pixels;
}

// The samples a draw may use of the composite: its buffer's, and under
// limits that cut a larger model no more than they keep at the draw's
// samples, however far a finer draw grew the buffer.
int64_t composite_plane(const ModelRenderer& renderer) noexcept {
    const auto plane = static_cast<int64_t>(renderer.composite.pixels.size() / 2);
    if (!renderer.composite_limits.clamp_oversize)
        return plane;
    return std::min(plane, clamped_plane(renderer));
}

std::size_t plane_size(const Sprite& sprite) noexcept {
    return static_cast<std::size_t>(sprite.width) * sprite.height;
}

// Projects a piece's vertices into image space, `samples` to a game pixel;
// depths stay in game pixels.
void project_image_vertices(
    const PieceState& piece,
    const Sprite& target,
    bool doubled,
    int32_t depth_base,
    uint32_t samples,
    std::vector<present::DepthVertex>& out
) {
    out.clear();
    for (const FixedVector3& v : piece.transformed_vertices) {
        int32_t x = hi_at(v.x, samples);
        int32_t y = hi_at(v.y, samples);
        int32_t z = hi_at(wrap_sub(0, v.z), samples);
        if (doubled) {
            x <<= 1;
            y <<= 1;
            z <<= 1;
        }
        present::DepthVertex vertex{};
        vertex.x = x + target.origin_x;
        vertex.y = z - (y >> 1) + target.origin_y;
        vertex.depth = depth_base + hi(v.y);
        out.push_back(vertex);
    }
}

// Starts the double-size pass of an anti-aliased building image in the
// composite buffer; returns the target to draw into. An image drawn finer
// than the game's pixels is drawn straight into.
Sprite*
begin_image_pass(ModelRenderer& renderer, Sprite& image, const ModelRef& model, int32_t pass) {
    if (renderer.samples != 1 || (renderer.graphics_flags & graphics_anti_alias) == 0 ||
        !is_building(*model.unit) || pass == 0)
        return &image;
    ensure_composite(renderer, static_cast<int64_t>(image.width) * 2 * image.height * 2);
    Sprite& composite = renderer.composite.sprite;
    const int64_t pixels = static_cast<int64_t>(image.width << 1) * (image.height << 1);
    if (pixels > composite_plane(renderer))
        return &image;
    composite.width = static_cast<uint16_t>(image.width << 1);
    composite.encoding = OA_SPRITE_RAW;
    composite.key = image_key;
    composite.height = static_cast<uint16_t>(image.height << 1);
    composite.origin_x = static_cast<int16_t>(static_cast<uint16_t>(image.origin_x) << 1);
    composite.origin_y = static_cast<int16_t>(static_cast<uint16_t>(image.origin_y) << 1);
    std::memset(composite.aux, 0, plane_size(composite));
    std::memset(composite.data, image_key, plane_size(composite));
    return &composite;
}

void finish_image_pass(ModelRenderer& renderer, Sprite& image, const Sprite* target) {
    if (target == &image || (renderer.graphics_flags & graphics_anti_alias) == 0)
        return;
    if (const present::DisplayContext* display = present::display_context(); display != nullptr)
        present::blend_downsample_sprite(*display, *target, image);
    copy_sparse_depth(*target, image);
}

void allocate_planes(present::SpriteBuffer& buffer, int32_t width, int32_t height, bool depth) {
    buffer = {};
    if (width < 0 || height < 0 || static_cast<int64_t>(width) * height > image_max_pixels)
        return;
    const auto size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    buffer.pixels.assign((depth ? size * 2 : size) + 1, 0);
    Sprite& sprite = buffer.sprite;
    sprite.width = static_cast<uint16_t>(width);
    sprite.height = static_cast<uint16_t>(height);
    sprite.data = buffer.pixels.data();
    sprite.aux = depth ? buffer.pixels.data() + size : nullptr;
    sprite.key = image_key;
    std::memset(sprite.data, image_key, size);
}

// Measures a model's visible pieces as measure_model_bounds does, `samples`
// to a game pixel, the margin included.
ImageFrame measure_bounds_at(
    const sim::model_runtime::Instance& instance, const FixedVector3* offset, uint32_t samples
) {
    int32_t min_x = 0;
    int32_t max_x = 0;
    int32_t min_y = 0;
    int32_t max_y = 0;
    const auto pieces = instance.pieces();
    for (std::size_t n = pieces.size(); n != 0; --n) {
        const PieceState& piece = pieces[n - 1];
        if ((piece.flags & piece_visible) == 0)
            continue;
        for (const FixedVector3& v : piece.transformed_vertices) {
            int32_t x = 0;
            int32_t y = 0;
            int32_t z = 0;
            if (offset == nullptr) {
                x = hi_at(v.x, samples);
                y = hi_at(v.y, samples);
                z = hi_at(wrap_sub(0, v.z), samples);
            } else {
                x = hi_at(wrap_add(v.x, offset->x), samples);
                z = hi_at(wrap_sub(offset->z, v.z), samples);
                y = hi_at(wrap_add(v.y, offset->y), samples);
            }
            const int32_t sy = z - (y >> 1);
            min_x = x < min_x ? x : min_x;
            max_x = max_x < x ? x : max_x;
            min_y = sy < min_y ? sy : min_y;
            max_y = max_y < sy ? sy : max_y;
        }
    }
    const int32_t margin = margin_at(samples);
    return {
        (max_x - (min_x - margin)) + margin,
        (max_y - (min_y - margin)) + margin,
        -(min_x - margin),
        -(min_y - margin)
    };
}

// Grows bounds as expand_model_bounds does, `samples` to a game pixel.
void expand_bounds_at(
    ModelBounds& bounds,
    const sim::model_runtime::Instance& instance,
    int32_t x,
    int32_t y,
    int32_t z,
    uint32_t samples
) {
    int32_t min_x = 0;
    int32_t max_x = 0;
    int32_t min_y = 0;
    int32_t max_y = 0;
    const auto pieces = instance.pieces();
    for (std::size_t n = pieces.size(); n != 0; --n) {
        const PieceState& piece = pieces[n - 1];
        if ((piece.flags & piece_visible) == 0)
            continue;
        for (const FixedVector3& v : piece.transformed_vertices) {
            const int32_t sx = hi_at(wrap_add(v.x, x), samples);
            const int32_t sy =
                hi_at(wrap_sub(z, v.z), samples) - (hi_at(wrap_add(v.y, y), samples) >> 1);
            min_x = sx < min_x ? sx : min_x;
            max_x = max_x < sx ? sx : max_x;
            min_y = sy < min_y ? sy : min_y;
            max_y = max_y < sy ? sy : max_y;
        }
    }
    const int32_t margin = margin_at(samples);
    if (min_x - margin < bounds.left)
        bounds.left = min_x - margin;
    if (min_y - margin < bounds.top)
        bounds.top = min_y - margin;
    if (bounds.right < max_x + margin)
        bounds.right = max_x + margin;
    if (bounds.bottom < max_y + margin)
        bounds.bottom = max_y + margin;
}

// Measures a model's sheared ground silhouette as measure_shadow_bounds
// does, `samples` to a game pixel.
ImageFrame shadow_bounds_at(const sim::model_runtime::Instance& instance, uint32_t samples) {
    int32_t min_x = 0;
    int32_t max_x = 0;
    int32_t min_y = 0;
    int32_t max_y = 0;
    const auto pieces = instance.pieces();
    for (std::size_t n = pieces.size(); n != 0; --n) {
        const PieceState& piece = pieces[n - 1];
        if ((piece.flags & piece_visible) == 0)
            continue;
        for (const FixedVector3& v : piece.transformed_vertices) {
            const int32_t lean = hi_at(v.y, samples) >> 2;
            const int32_t sx = hi_at(v.x, samples) + lean;
            const int32_t sy = hi_at(wrap_sub(0, v.z), samples) - lean;
            min_x = sx < min_x ? sx : min_x;
            max_x = max_x < sx ? sx : max_x;
            min_y = sy < min_y ? sy : min_y;
            max_y = max_y < sy ? sy : max_y;
        }
    }
    const int32_t margin = margin_at(samples);
    return {
        (max_x - (min_x - margin)) + margin,
        (max_y - (min_y - margin)) + margin,
        -(min_x - margin),
        -(min_y - margin)
    };
}

// Fills the sheared silhouette as draw_shadow_silhouette does, `samples` to
// a game pixel; depths stay in game pixels.
void shadow_silhouette_at(Sprite& target, const ModelRef& model, uint32_t samples) {
    thread_local std::vector<present::DepthVertex> projected;
    thread_local std::vector<present::DepthVertex> corners;
    const formats::objects3d::Model& source = model.instance->model();
    const auto pieces = model.instance->pieces();
    for (std::size_t n = pieces.size(); n != 0; --n) {
        const PieceState& piece = pieces[n - 1];
        if ((piece.flags & piece_visible) == 0 || (piece.flags & piece_cached) == 0)
            continue;
        projected.clear();
        for (const FixedVector3& v : piece.transformed_vertices) {
            const int32_t height = hi(v.y);
            const int32_t lean = hi_at(v.y, samples) >> 2;
            projected.push_back(
                {hi_at(v.x, samples) + lean + target.origin_x,
                 hi_at(wrap_sub(0, v.z), samples) - lean + target.origin_y,
                 height + shadow_depth_base}
            );
        }
        const formats::objects3d::Object& object = source.objects[piece.object_index];
        const PreparedObject& prepared = model.prepared->objects[piece.object_index];
        for (uint32_t i = first_primitive(prepared); i < prepared.primitives.size(); ++i) {
            if (gather(source_of(object, prepared.primitives[i]), projected, corners))
                present::fill_depth_polygon(
                    target, corners.data(), static_cast<int32_t>(corners.size()), 0
                );
        }
    }
}

// Outlines the visible pieces as outline_model does, `samples` to a game
// pixel: each edge is drawn `samples` times, a sample further right each
// time, so that it stays a game pixel wide.
void outline_at(Sprite& image, const ModelRef& model, uint8_t color, uint32_t samples) {
    thread_local std::vector<present::DepthVertex> projected;
    thread_local std::vector<present::DepthVertex> corners;
    const formats::objects3d::Model& source = model.instance->model();
    const int32_t base = vertex_depth_base(model);
    const auto pieces = model.instance->pieces();
    for (std::size_t n = pieces.size(); n != 0; --n) {
        const PieceState& piece = pieces[n - 1];
        if ((piece.flags & piece_visible) == 0)
            continue;
        project_image_vertices(piece, image, false, base, samples, projected);
        const formats::objects3d::Object& object = source.objects[piece.object_index];
        const PreparedObject& prepared = model.prepared->objects[piece.object_index];
        for (uint32_t i = first_primitive(prepared); i < prepared.primitives.size(); ++i) {
            if (!gather(source_of(object, prepared.primitives[i]), projected, corners) ||
                corners.empty())
                continue;
            corners.push_back(corners.front());
            for (uint32_t shift = 0; shift < samples; ++shift) {
                if (shift != 0)
                    for (present::DepthVertex& corner : corners)
                        ++corner.x;
                present::outline_depth_polygon(
                    image, corners.data(), static_cast<int32_t>(corners.size()), color
                );
            }
        }
    }
}

// The part of a unit's draw from its image a pass does: everything as one
// draw (draw_unit_model), only what it builds (plan_model_draw), or only
// what it draws (draw_planned_model).
enum class Pass : uint8_t { whole, plan, draw };

// A carried unit's entry in a plan, or null.
const CarriedDraw* carried_in(const ModelDrawPlan& plan, const Unit& unit) noexcept {
    for (const CarriedDraw& carried : plan.carried)
        if (carried.unit == &unit)
            return &carried;
    return nullptr;
}

// The model a draw takes for a unit its model carries: the renderer's
// model_of, or a plan's note of it when the draw is drawn from a plan; a
// null instance when there is none.
ModelRef
carried_model(const ModelRenderer& renderer, const ModelDrawPlan* planned, const Unit& unit) {
    if (planned != nullptr) {
        const CarriedDraw* carried = carried_in(*planned, unit);
        return carried != nullptr ? carried->model : ModelRef{};
    }
    if (renderer.model_of == nullptr)
        return {};
    return renderer.model_of(renderer.user, unit);
}

// A model as the composer callbacks see it.
struct ComposeContext {
    ModelRenderer* renderer{};
    const ModelRef* model{};
    const Sprite* source{};
    /// The plan the draw is drawn from, which names the carried units'
    /// models; null to ask the renderer's model_of.
    const ModelDrawPlan* planned{};
};

bool compose_bounds(void* user, const Unit& unit, SpriteBounds& out) {
    auto* context = static_cast<ComposeContext*>(user);
    ModelRef model = *context->model;
    if (&unit != context->model->unit) {
        if (context->planned == nullptr && context->renderer->model_of == nullptr)
            return false;
        model = carried_model(*context->renderer, context->planned, unit);
    }
    if (model.instance == nullptr)
        return false;
    // The composer places a carried unit's extent by `unit`, the record it
    // walked to; the unit draws where the record model_of handed back stands,
    // which a draw between ticks places elsewhere.
    ModelBounds bounds{};
    expand_bounds_at(
        bounds,
        *model.instance,
        wrap_sub(model.unit->position.x, unit.position.x),
        wrap_sub(model.unit->position.y, unit.position.y),
        wrap_sub(model.unit->position.z, unit.position.z),
        context->renderer->samples
    );
    out = {bounds.left, bounds.right, bounds.top, bounds.bottom};
    return true;
}

void set_composite_frame(ModelRenderer& renderer, const SpriteFrame& frame, uint8_t key) {
    ensure_composite(renderer, static_cast<int64_t>(frame.width) * frame.height);
    Sprite& composite = renderer.composite.sprite;
    composite.width = frame.width;
    composite.height = frame.height;
    if (renderer.composite_limits.clamp_oversize) {
        // A model larger than the buffer keeps its top left part: the part
        // the limits hold in game pixels, at whatever samples it is drawn.
        composite.width = static_cast<uint16_t>(std::min<int32_t>(
            composite.width, clamped_side(renderer.composite_limits.width, renderer.samples)
        ));
        composite.height = static_cast<uint16_t>(std::min<int32_t>(
            composite.height, clamped_side(renderer.composite_limits.height, renderer.samples)
        ));
        // Where the buffer could not grow that far, the rows it holds.
        if (composite.width != 0 &&
            static_cast<int64_t>(plane_size(composite)) > composite_plane(renderer))
            composite.height = static_cast<uint16_t>(composite_plane(renderer) / composite.width);
    }
    composite.origin_x = frame.origin_x;
    composite.origin_y = frame.origin_y;
    composite.key = key;
}

void compose_copy(void* user, const SpriteFrame& frame) {
    auto* context = static_cast<ComposeContext*>(user);
    const Sprite& source = *context->source;
    set_composite_frame(*context->renderer, frame, source.key);
    Sprite& composite = context->renderer->composite.sprite;
    if (composite.width != source.width || composite.height != source.height) {
        // Cut to the buffer: the top left rows and columns of the image.
        if (composite.width > source.width || composite.height > source.height)
            return;
        auto* data = static_cast<uint8_t*>(composite.data);
        auto* aux = static_cast<uint8_t*>(composite.aux);
        for (uint32_t row = 0; row < composite.height; ++row) {
            const std::size_t from = std::size_t{row} * source.width;
            const std::size_t to = std::size_t{row} * composite.width;
            std::memmove(
                data + to, static_cast<const uint8_t*>(source.data) + from, composite.width
            );
            if (source.aux != nullptr)
                std::memmove(
                    aux + to, static_cast<const uint8_t*>(source.aux) + from, composite.width
                );
        }
        return;
    }
    const auto size = plane_size(source);
    if (static_cast<int64_t>(size) > composite_plane(*context->renderer))
        return;
    std::memmove(composite.data, source.data, size);
    if (source.aux != nullptr)
        std::memmove(composite.aux, source.aux, size);
}

// Clears the composite and draws the source's colour plane, then its depth
// plane (as a keyed sprite), at (x, y).
void compose_redraw(void* user, const SpriteFrame& frame, int32_t x, int32_t y) {
    auto* context = static_cast<ComposeContext*>(user);
    Sprite source = *context->source;
    set_composite_frame(*context->renderer, frame, source.key);
    Sprite& composite = context->renderer->composite.sprite;
    if (static_cast<int64_t>(plane_size(composite)) > composite_plane(*context->renderer))
        return;
    source.origin_x = 0;
    source.origin_y = 0;
    Surface surface{};
    present::surface_from_sprite(surface, composite);
    std::memset(composite.data, composite.key, plane_size(composite));
    std::memset(composite.aux, 0, plane_size(composite));
    present::draw_sprite(&surface, &source, x, y);
    if (source.aux == nullptr)
        return;
    surface.pixels = static_cast<uint8_t*>(composite.aux);
    std::swap(source.data, source.aux);
    present::draw_sprite(&surface, &source, x, y);
}

// The build effect runs at the end of the composition.
void compose_finish(void* user, const SpriteFrame&) {
    auto* context = static_cast<ComposeContext*>(user);
    apply_build_effect(*context->renderer, context->renderer->composite.sprite, *context->model);
}

// Returns the table a renderer's shadows blend through: its faded table,
// else the display's alpha table; null when there is neither.
const uint8_t* shadow_blend_table(const ModelRenderer& renderer) {
    if (renderer.shadow_table != nullptr)
        return renderer.shadow_table;
    const present::DisplayContext* display = present::display_context();
    if (display == nullptr || (display->flags & present::display_flag_alpha_table) == 0)
        return nullptr;
    return display->alpha_table;
}

// Draws a shadow sprite (a silhouette or a building's shadow image) blended
// through the renderer's shadow table: what draw_sprite_blended draws with
// the display's own.
void draw_shadow_sprite(
    const ModelRenderer& renderer, Surface* target, const Sprite* sprite, int32_t x, int32_t y
) {
    if (renderer.shadow_table == nullptr)
        present::draw_sprite_blended(target, sprite, x, y);
    else
        present::draw_sprite_blended_through(target, sprite, x, y, renderer.shadow_table);
}

// Blends colour 0 through the renderer's shadow table into the target pixels
// under a raw image's opaque pixels, in one pass: what draw_shadow_sprite
// draws of the image's colour-0 silhouette (copy_silhouette).
void draw_silhouette_blended(
    const ModelRenderer& renderer, Surface& target, const Sprite& image, int32_t x, int32_t y
) {
    const uint8_t* table = shadow_blend_table(renderer);
    if (table == nullptr || image.data == nullptr)
        return;
    Rect32 source{0, 0, image.width - 1, image.height - 1};
    Rect32 placed{
        x - image.origin_x,
        y - image.origin_y,
        x - image.origin_x + image.width - 1,
        y - image.origin_y + image.height - 1
    };
    present::trim_to_surface(source, placed, target);
    if (placed.x1 > placed.x2 || placed.y1 > placed.y2 || source.x1 > source.x2 ||
        source.y1 > source.y2)
        return;
    Surface mask{};
    mask.width = image.width;
    mask.height = image.height;
    mask.pitch = image.width;
    mask.pixels = static_cast<uint8_t*>(image.data);
    // Row 0 of the table: colour 0 over each target colour.
    present::remap_under_mask(target, mask, source, placed, image.key, table);
}

// Grows sprite bounds to hold a part.
void include_part(SpriteBounds& into, const SpriteBounds& part) noexcept {
    into.left = std::min(into.left, part.left);
    into.right = std::max(into.right, part.right);
    into.top = std::min(into.top, part.top);
    into.bottom = std::max(into.bottom, part.bottom);
}

// Composes a unit's sprite as compose_unit_sprite does, with every
// extent and offset counted in samples (ModelRenderer::samples): the frame
// that holds the image and the models of the unit and the units it carries,
// the image copied or redrawn into it, then the build effect.
void compose_finer_sprite(ComposeContext& context, const Unit& unit, const SpriteFrame& source) {
    const uint32_t samples = context.renderer->samples;
    World& world = *context.renderer->world;
    SpriteBounds bounds{0, 0, 0, 0};
    SpriteBounds extent{};
    if (compose_bounds(&context, unit, extent))
        include_part(bounds, extent);
    for (const Unit* child = world_unit(&world, unit.attach_first_child); child != nullptr;
         child = world_unit(&world, child->attach_next)) {
        if ((child->flags & unit_flag_attached_without_piece) != 0)
            continue;
        SpriteBounds part{0, 0, 0, 0};
        if (compose_bounds(&context, *child, extent))
            include_part(part, extent);
        const int32_t across = hi_at(wrap_sub(child->position.x, unit.position.x), samples);
        const int32_t lifted = hi_at(wrap_sub(child->position.z, unit.position.z), samples) -
                               (hi_at(wrap_sub(child->position.y, unit.position.y), samples) >> 1);
        include_part(
            bounds,
            {part.left + across, part.right + across, part.top + lifted, part.bottom + lifted}
        );
    }
    const SpriteFrame frame = unit_sprite_frame(source, &bounds, 1);
    if (frame.width == source.width && frame.height == source.height)
        compose_copy(&context, frame);
    else
        compose_redraw(
            &context, frame, frame.origin_x - source.origin_x, frame.origin_y - source.origin_y
        );
    compose_finish(&context, frame);
}

// Draws coloured primitives filled and textured quads mapped, from vertices
// already on the target surface.
template <typename Texture>
void draw_flat_primitives(
    Surface* target,
    const formats::objects3d::Object& object,
    const PreparedObject& prepared,
    const std::vector<present::PolygonVertex>& projected,
    Texture texture
) {
    thread_local std::vector<present::PolygonVertex> corners;
    for (uint32_t i = first_primitive(prepared); i < prepared.primitives.size(); ++i) {
        const PreparedPrimitive& primitive = prepared.primitives[i];
        if (!gather(source_of(object, primitive), projected, corners))
            continue;
        const auto count = static_cast<int32_t>(corners.size());
        if ((primitive.flags & primitive_colored) != 0) {
            present::fill_polygon(target, corners.data(), count, primitive.color);
        } else if (count == 4) {
            if (const Sprite* sprite = texture(primitive); sprite != nullptr)
                texture_quad(target, sprite, corners.data(), nullptr);
        }
    }
}

} // namespace

bool init_composite_buffer(ModelRenderer& renderer) {
    // Allocated here rather than by create_two_plane_sprite, whose bound of
    // 16 Mi pixels a profile's largest composite passes.
    const int32_t width = std::max(renderer.composite_limits.width, int32_t{0});
    const int32_t height = std::max(renderer.composite_limits.height, int32_t{0});
    const auto plane = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    renderer.composite = {};
    renderer.composite.pixels.assign(plane * 2, 0);
    renderer.composite.sprite.width = static_cast<uint16_t>(width);
    renderer.composite.sprite.height = static_cast<uint16_t>(height);
    renderer.composite.sprite.data = plane != 0 ? renderer.composite.pixels.data() : nullptr;
    renderer.composite.sprite.aux = plane != 0 ? renderer.composite.pixels.data() + plane : nullptr;
    // Every image copied into the composite uses key 1.
    renderer.composite.sprite.key = image_key;
    return renderer.composite.sprite.data != nullptr;
}

void copy_renderer_settings(ModelRenderer& from, ModelRenderer& to) {
    // Both composites are set aside, so that the assignment copies every
    // other field, whatever fields the renderer gains, and no pixels.
    present::SpriteBuffer kept;
    present::SpriteBuffer lent;
    std::swap(kept, to.composite);
    std::swap(lent, from.composite);
    to = from;
    std::swap(from.composite, lent);
    std::swap(to.composite, kept);
    if (to.composite.sprite.data == nullptr)
        init_composite_buffer(to);
}

void set_model_light(ModelRenderer& renderer, int32_t x, int32_t y, int32_t z) noexcept {
    renderer.light[0] = static_cast<float>(static_cast<double>(x) * light_input_scale);
    renderer.light[1] = static_cast<float>(static_cast<double>(y) * light_input_scale);
    renderer.light[2] = static_cast<float>(static_cast<double>(z) * light_input_scale);
}

ImageFrame
measure_model_bounds(const sim::model_runtime::Instance& instance, const FixedVector3* offset) {
    return measure_bounds_at(instance, offset, 1);
}

void expand_model_bounds(
    ModelBounds& bounds,
    const sim::model_runtime::Instance& instance,
    int32_t x,
    int32_t y,
    int32_t z
) {
    expand_bounds_at(bounds, instance, x, y, z, 1);
}

void draw_model(
    ModelRenderer& renderer,
    Surface* target,
    const ModelRef& model,
    int32_t camera_x,
    int32_t camera_z,
    bool first_frame
) {
    if (has_image(*model.state)) {
        draw_unit_model(renderer, target, model, camera_x, camera_z, first_frame);
        return;
    }
    const formats::objects3d::Model& source = model.instance->model();
    const auto pieces = model.instance->pieces();
    for (std::size_t n = pieces.size(); n != 0; --n) {
        const PieceState& piece = pieces[n - 1];
        if ((piece.flags & piece_visible) != 0)
            draw_piece_flat(
                renderer,
                target,
                *model.unit,
                camera_x,
                camera_z,
                source.objects[piece.object_index],
                model.prepared->objects[piece.object_index],
                piece,
                model.unit->owner_index,
                first_frame
            );
    }
}

void draw_piece_flat(
    const ModelRenderer& renderer,
    Surface* target,
    const Unit& unit,
    int32_t camera_x,
    int32_t camera_z,
    const formats::objects3d::Object& object,
    const PreparedObject& prepared,
    const PieceState& piece,
    uint8_t owner,
    bool first_frame
) {
    thread_local std::vector<present::PolygonVertex> projected;
    const int32_t dx = wrap_sub(unit.position.x, camera_x);
    const int32_t dz = wrap_sub(unit.position.z, camera_z);
    const uint32_t samples = renderer.samples;
    projected.clear();
    for (const FixedVector3& v : piece.transformed_vertices) {
        present::PolygonVertex vertex{};
        vertex.x = hi_at(wrap_add(v.x, dx), samples) + renderer.origin_x;
        vertex.y = (hi_at(wrap_sub(dz, v.z), samples) -
                    (hi_at(wrap_add(v.y, unit.position.y), samples) >> 1)) +
                   renderer.origin_y;
        projected.push_back(vertex);
    }
    const uint8_t team = renderer.team_colors[owner % 10];
    draw_flat_primitives(
        target, object, prepared, projected, [&](const PreparedPrimitive& primitive) {
            return primitive_texture(primitive, first_frame, team);
        }
    );
}

void allocate_image(present::SpriteBuffer& image, int32_t width, int32_t height) {
    allocate_planes(image, width, height, false);
}

void allocate_depth_image(present::SpriteBuffer& image, int32_t width, int32_t height) {
    allocate_planes(image, width, height, true);
}

namespace {

// Measures a model and builds its image into `image`, as prepare_model_image
// builds the draw state's; false when the image cannot be allocated.
bool build_image_into(
    ModelRenderer& renderer,
    const ModelRef& model,
    bool attached,
    int32_t pass,
    present::SpriteBuffer& image
) {
    const Unit& unit = *model.unit;
    const ImageFrame frame = measure_bounds_at(*model.instance, nullptr, renderer.samples);
    if (!attached && (unit.flags2 & OA_UNIT_FLAG2_Z_BUFFER) == 0 && unit.build_remaining == 0.0F)
        allocate_image(image, frame.width, frame.height);
    else
        allocate_depth_image(image, frame.width, frame.height);
    if (image.sprite.data == nullptr)
        return false;
    image.sprite.origin_x = static_cast<int16_t>(frame.origin_x);
    image.sprite.origin_y = static_cast<int16_t>(frame.origin_y);
    if (!is_building(unit) || (renderer.graphics_flags & graphics_shading) == 0)
        build_model_image(renderer, image.sprite, model, unit.owner_index, pass);
    else
        build_shaded_model_image(renderer, image.sprite, model, unit.owner_index, pass);
    return true;
}

// Tells whether a model's image build goes through the renderer's composite
// (begin_image_pass): a building's image drawn double size with
// anti-aliasing on, at the game's pixels.
bool builds_in_composite(const ModelRenderer& renderer, const ModelRef& model, int32_t pass) {
    return renderer.samples == 1 && (renderer.graphics_flags & graphics_anti_alias) != 0 &&
           is_building(*model.unit) && pass != 0;
}

// Copies an image and its pixels.
void copy_image(const present::SpriteBuffer& from, present::SpriteBuffer& to) {
    to.pixels = from.pixels;
    to.sprite = from.sprite;
    to.sprite.data = to.pixels.data();
    if (from.sprite.aux != nullptr)
        to.sprite.aux =
            to.pixels.data() + (static_cast<const uint8_t*>(from.sprite.aux) - from.pixels.data());
}

} // namespace

bool prepare_model_image(
    ModelRenderer& renderer, const ModelRef& model, bool attached, int32_t pass
) {
    ModelState& state = *model.state;
    if (!build_image_into(renderer, model, attached, pass, state.image))
        return false;
    state.image_unfinished = model.unit->build_remaining != 0.0F;
    ++state.image_builds;
    return true;
}

void copy_sparse_depth(const Sprite& source, Sprite& target) noexcept {
    auto* out = static_cast<uint8_t*>(target.aux);
    if (out == nullptr || source.aux == nullptr)
        return;
    const auto* in = static_cast<const uint8_t*>(source.aux);
    for (int32_t y = 0; y < target.height; ++y) {
        for (int32_t x = 0; x < target.width; ++x) {
            *out++ = *in;
            in += 2;
        }
        in += source.width;
    }
}

void remap_depth_bands(
    Sprite& image, uint8_t threshold, int32_t above, int32_t below, int32_t band
) noexcept {
    const uint8_t low = threshold < 4 ? 0 : static_cast<uint8_t>(threshold - 4);
    remap_depth_range(image, low, threshold, above, below, band);
}

void remap_depth_range(
    Sprite& image, uint8_t low, uint8_t end, int32_t above, int32_t below, int32_t band
) noexcept {
    auto* pixels = static_cast<uint8_t*>(image.data);
    const auto* depth = static_cast<const uint8_t*>(image.aux);
    const int32_t count = static_cast<int32_t>(image.width) * image.height;
    for (int32_t i = 0; i < count; ++i) {
        if (pixels[i] == image.key)
            continue;
        int32_t value = below;
        if (low <= depth[i]) {
            value = above;
            if (depth[i] < end)
                value = band;
        }
        if (value == remap_clear)
            pixels[i] = image.key;
        else if (value != remap_keep)
            pixels[i] = static_cast<uint8_t>(value);
    }
}

BuildPulseColours build_pulse_colours(uint32_t pulse_tick, uint32_t unit_id) noexcept {
    // The products wrap at 32 bits, as the game's do.
    const uint32_t first =
        (pulse_tick * nano_wave_first_steps) / nano_wave_period + (unit_id ^ nano_wave_first_salt);
    const uint32_t second = (pulse_tick * nano_wave_second_steps) / nano_wave_period +
                            (unit_id ^ nano_wave_second_salt);
    const auto ramp = [](uint32_t wave) -> uint8_t {
        return static_cast<uint8_t>(
            (wave & nano_ramp_turn) != 0 ? nano_ramp_high - (wave & nano_ramp_mask)
                                         : nano_ramp_low + (wave & nano_ramp_mask)
        );
    };
    return {ramp(first), ramp(second)};
}

float build_pulse_rate(float zoom) noexcept {
    if (!(zoom < build_pulse_full_rate_zoom))
        return 1.0F;
    return std::max(zoom, build_pulse_least_rate);
}

uint32_t advance_build_pulse(BuildPulseClock& clock, uint32_t game_tick, float zoom) noexcept {
    const float rate = build_pulse_rate(zoom);
    if (!clock.seen || game_tick < clock.last_tick || rate >= 1.0F) {
        clock.seen = true;
        clock.last_tick = game_tick;
        clock.lag = 0;
        clock.lag_fraction = 0;
        return 0;
    }
    const uint64_t ticks = game_tick - clock.last_tick;
    clock.last_tick = game_tick;
    const auto rate_fraction =
        static_cast<uint32_t>(std::lround(rate * static_cast<float>(pulse_fraction_one)));
    const uint64_t gathered =
        uint64_t{clock.lag_fraction} + ticks * (pulse_fraction_one - rate_fraction);
    clock.lag += static_cast<uint32_t>(gathered >> pulse_fraction_bits);
    clock.lag_fraction = static_cast<uint32_t>(gathered & (pulse_fraction_one - 1));
    return clock.lag;
}

bool apply_build_effect(const ModelRenderer& renderer, Sprite& image, const ModelRef& model) {
    const Unit& unit = *model.unit;
    if (image.aux == nullptr || unit.build_remaining == 0.0F)
        return false;
    if (model.state != nullptr && model.state->build_bands) {
        const BuildEffectBands& bands = *model.state->build_bands;
        remap_depth_range(
            image, bands.band_low, bands.band_end, bands.above, bands.below, bands.band
        );
        outline_at(image, model, bands.outline, renderer.samples);
        return true;
    }
    const BuildPulseColours colours =
        build_pulse_colours(renderer.tick - renderer.build_pulse_lag, unit.id);
    const int32_t color_a = colours.first;
    const int32_t color_b = colours.second;
    const int32_t progress =
        truncate_low32(static_cast<double>(unit.build_remaining) * progress_scale);
    if (progress > 0xeb) {
        remap_depth_bands(
            image,
            static_cast<uint8_t>(((progress - 0xeb) * 0xff) / 0x14),
            remap_clear,
            remap_clear,
            color_a
        );
    } else if (progress > 0xc8) {
        remap_depth_bands(
            image,
            static_cast<uint8_t>(((progress - 0xc8) * 0xff) / 0x23),
            remap_clear,
            remap_clear,
            color_a
        );
    } else if (progress > 0x73) {
        remap_depth_bands(
            image,
            static_cast<uint8_t>(((0x73 - progress) * 0xff) / 0x55 - 1),
            remap_clear,
            color_a,
            color_b
        );
    } else if (progress > 0x1e) {
        remap_depth_bands(
            image,
            static_cast<uint8_t>(((0x1e - progress) * 0xff) / 0x55 - 1),
            color_a,
            remap_keep,
            color_b
        );
    } else {
        remap_depth_bands(
            image, static_cast<uint8_t>((progress * 0xff) / 0x1e), remap_keep, remap_keep, color_a
        );
    }
    outline_at(image, model, static_cast<uint8_t>(color_b), renderer.samples);
    return true;
}

void outline_model(Sprite& image, const ModelRef& model, uint8_t color) {
    outline_at(image, model, color, 1);
}

namespace {

// A unit's draw from its cached image, or the part of it `pass` names: with
// Pass::plan it builds what the draw builds and notes what the draw needs in
// `noted`, drawing nothing; with Pass::draw it draws from `planned`,
// building nothing; with Pass::whole it does both in the draw's order, as
// draw_unit_model.
void unit_model_pass(
    ModelRenderer& renderer,
    Surface* target,
    const ModelRef& model,
    int32_t camera_x,
    int32_t camera_z,
    bool first_frame,
    Pass pass,
    ModelDrawPlan* noted,
    const ModelDrawPlan* planned
) {
    ModelState& state = *model.state;
    if (!has_image(state))
        return;
    const bool builds = pass != Pass::draw;
    const bool draws = pass != Pass::plan;
    const Unit& unit = *model.unit;
    const Game& game = renderer.world->game;
    const int32_t dx = wrap_sub(unit.position.x, camera_x);
    const int32_t dz = wrap_sub(unit.position.z, camera_z);
    int32_t ground = 0;
    if (planned != nullptr)
        ground = planned->ground;
    else if (renderer.ground_height != nullptr)
        ground = renderer.ground_height(renderer.user, unit.position);
    if (noted != nullptr)
        noted->ground = ground;
    // Places are counted in samples; heights and depths stay in game pixels.
    const uint32_t samples = renderer.samples;
    const auto scale = static_cast<int32_t>(samples);
    const int32_t unit_height = hi(unit.position.y);
    const int32_t sx = hi_at(dx, samples);
    const int32_t shadow_y = hi_at(dz, samples) - ((ground * scale) >> 1) + renderer.origin_y;
    const int32_t unit_y =
        hi_at(dz, samples) - (hi_at(unit.position.y, samples) >> 1) + renderer.origin_y;
    const int32_t shadow_x = sx + renderer.origin_x + shadow_offset_x * scale;
    const int32_t image_x = sx + renderer.origin_x;
    const uint32_t flags = def_flags(model);
    const bool shadows = (renderer.graphics_flags & graphics_shadows) != 0 &&
                         (flags & OA_UNIT_DEF_FLAG_NO_SHADOW) == 0;
    const bool vehicle_shadow =
        (renderer.graphics_flags & graphics_vehicle_shadows) != 0 &&
        (flags & (OA_UNIT_DEF_FLAG_FLOATER | OA_UNIT_DEF_FLAG_CAN_HOVER)) == 0;
    const bool submerged_standin =
        unit.type_index == 0 && static_cast<int16_t>(unit_height) < game.sea_level;
    const bool translucent =
        (unit.state_flags & unit_state_cloaked) != 0 || game.debug_overlay != 0;
    const auto draw_building_shadow = [&]() {
        if (submerged_standin)
            return;
        if (builds && state.shadow.sprite.data == nullptr)
            build_shadow_image(renderer, model, state.image.sprite);
        if (draws && state.shadow.sprite.data != nullptr)
            draw_shadow_sprite(renderer, target, &state.shadow.sprite, shadow_x, shadow_y);
    };
    // The units it carries, in the order the draw walks them; a plan notes
    // each with the model the renderer hands back.
    const auto walk_carried = [&](auto&& visit) {
        for (const Unit* child = world_unit(renderer.world, unit.attach_first_child);
             child != nullptr;
             child = world_unit(renderer.world, child->attach_next)) {
            if ((child->flags & unit_flag_attached_without_piece) != 0 ||
                (planned == nullptr && renderer.model_of == nullptr))
                continue;
            const ModelRef carried = carried_model(renderer, planned, *child);
            if (noted != nullptr) {
                noted->carried.emplace_back();
                noted->carried.back().unit = child;
                noted->carried.back().model = carried;
            }
            if (carried.instance == nullptr)
                continue;
            visit(*child, carried);
        }
    };
    if (state.image.sprite.aux == nullptr) {
        if (shadows) {
            if (is_building(unit) && (flags & OA_UNIT_DEF_FLAG_DIGGER) == 0)
                draw_building_shadow();
            else if (draws && vehicle_shadow && samples != 1 && target != nullptr)
                draw_silhouette_blended(renderer, *target, state.image.sprite, shadow_x, shadow_y);
            else if (draws && vehicle_shadow)
                draw_shadow_sprite(
                    renderer,
                    target,
                    &copy_silhouette(renderer, state.image.sprite),
                    shadow_x,
                    shadow_y
                );
        }
        if (!has_image(state) &&
            (!builds || !prepare_model_image(renderer, model, false, pass_cached_pieces)))
            return;
        if (draws) {
            if (!translucent)
                present::draw_sprite(target, &state.image.sprite, image_x, unit_y);
            else
                present::draw_sprite_blended(target, &state.image.sprite, image_x, unit_y);
            const formats::objects3d::Model& source = model.instance->model();
            const auto pieces = model.instance->pieces();
            for (std::size_t n = pieces.size(); n != 0; --n) {
                const PieceState& piece = pieces[n - 1];
                if ((piece.flags & piece_visible) != 0 && (piece.flags & piece_cached) == 0)
                    draw_piece_flat(
                        renderer,
                        target,
                        unit,
                        camera_x,
                        camera_z,
                        source.objects[piece.object_index],
                        model.prepared->objects[piece.object_index],
                        piece,
                        unit.owner_index,
                        first_frame
                    );
            }
        }
        walk_carried([&](const Unit&, const ModelRef& carried) {
            if (!draws)
                return;
            const formats::objects3d::Model& child_source = carried.instance->model();
            const auto child_pieces = carried.instance->pieces();
            for (std::size_t n = child_pieces.size(); n != 0; --n) {
                const PieceState& piece = child_pieces[n - 1];
                if ((piece.flags & piece_visible) != 0)
                    draw_piece_flat(
                        renderer,
                        target,
                        *carried.unit,
                        camera_x,
                        camera_z,
                        child_source.objects[piece.object_index],
                        carried.prepared->objects[piece.object_index],
                        piece,
                        carried.unit->owner_index,
                        first_frame
                    );
            }
        });
        return;
    }
    if (shadows) {
        if ((flags & OA_UNIT_DEF_FLAG_DIGGER) != 0) {
            if (draws) {
                Sprite& silhouette = copy_silhouette(renderer, state.image.sprite);
                present::clear_sprite_below_depth(
                    silhouette, static_cast<uint8_t>(vertex_depth_base(model))
                );
                draw_shadow_sprite(renderer, target, &silhouette, shadow_x, shadow_y);
            }
        } else if (is_building(unit)) {
            draw_building_shadow();
        } else if (
            vehicle_shadow && static_cast<int32_t>(game.sea_level) - unit_height <= 0 &&
            samples != 1 && target != nullptr
        ) {
            if (draws)
                draw_silhouette_blended(renderer, *target, state.image.sprite, shadow_x, shadow_y);
        } else if (vehicle_shadow) {
            if (draws) {
                Sprite& silhouette = copy_silhouette(renderer, state.image.sprite);
                const int32_t lift = static_cast<int32_t>(game.sea_level) - unit_height;
                if (lift > 0)
                    present::clear_sprite_below_depth(
                        silhouette, static_cast<uint8_t>(vertex_depth_base(model) + lift)
                    );
                draw_shadow_sprite(renderer, target, &silhouette, shadow_x, shadow_y);
            }
        }
    }
    if (!has_image(state) &&
        (!builds || !prepare_model_image(renderer, model, false, pass_cached_pieces)))
        return;
    const Sprite& image = state.image.sprite;
    Sprite& composite = renderer.composite.sprite;
    if (draws) {
        ComposeContext context{&renderer, &model, &image, planned};
        SpriteComposer composer{};
        composer.user = &context;
        composer.model_bounds = compose_bounds;
        composer.copy = compose_copy;
        composer.redraw = compose_redraw;
        composer.finish = compose_finish;
        const SpriteFrame source_frame{image.width, image.height, image.origin_x, image.origin_y};
        if (samples == 1)
            compose_unit_sprite(*renderer.world, unit, source_frame, composer);
        else
            compose_finer_sprite(context, unit, source_frame);
        if ((!is_building(unit) && !renderer.moving_pieces_once_built) ||
            unit.build_remaining == 0.0F)
            build_model_image(renderer, composite, model, unit.owner_index, pass_moving_pieces);
    }
    // Each carried unit's image, built as carried with every piece and the
    // build effect, goes into the composite at its place.
    thread_local present::SpriteBuffer rebuilt;
    walk_carried([&](const Unit& child, const ModelRef& carried) {
        const Sprite* carried_image = nullptr;
        if (planned != nullptr) {
            const CarriedDraw* entry = carried_in(*planned, child);
            if (entry->built_in_composite) {
                // Built again, as the draw built it, for what it leaves in
                // the composite.
                if (!build_image_into(renderer, carried, true, pass_all_pieces, rebuilt))
                    return;
                apply_build_effect(renderer, rebuilt.sprite, carried);
                carried_image = &rebuilt.sprite;
            } else {
                if (entry->image.sprite.data == nullptr)
                    return;
                carried_image = &entry->image.sprite;
            }
        } else {
            // A plan notes the child last, just before this visit.
            CarriedDraw* entry = noted != nullptr ? &noted->carried.back() : nullptr;
            if (entry != nullptr)
                entry->built_in_composite = builds_in_composite(renderer, carried, pass_all_pieces);
            prepare_model_image(renderer, carried, true, pass_all_pieces);
            if (!has_image(*carried.state))
                return;
            apply_build_effect(renderer, carried.state->image.sprite, carried);
            if (entry != nullptr)
                copy_image(carried.state->image, entry->image);
            carried_image = &carried.state->image.sprite;
        }
        if (!draws)
            return;
        const int32_t cx = wrap_sub(carried.unit->position.x, unit.position.x);
        const int32_t cy = wrap_sub(carried.unit->position.y, unit.position.y);
        const int32_t cz = wrap_sub(carried.unit->position.z, unit.position.z);
        present::composite_depth_sprite(
            *carried_image,
            composite,
            hi_at(cx, samples),
            hi_at(cz, samples) - (hi_at(cy, samples) >> 1),
            hi(cy)
        );
    });
    if (!draws)
        return;
    const int32_t lift = static_cast<int32_t>(game.sea_level) - unit_height;
    if (lift > 0) {
        const auto threshold = static_cast<uint8_t>(lift + vertex_depth_base(model));
        if ((unit.flags & OA_UNIT_FLAG_VIEWPOINT_OWNED) == 0 &&
            unit.owner_index != game.viewpoint_player)
            present::clear_sprite_below_depth(composite, threshold);
        else
            present::tint_sprite_below_depth(composite, threshold);
    }
    if ((flags & OA_UNIT_DEF_FLAG_DIGGER) != 0)
        present::clear_sprite_below_depth(composite, digger_clip_depth);
    if (!translucent)
        present::draw_sprite(target, &composite, image_x, unit_y);
    else
        present::draw_sprite_blended(target, &composite, image_x, unit_y);
}

// Draws a model's visible pieces flat (draw_model without a cached image).
void draw_model_pieces(
    const ModelRenderer& renderer,
    Surface* target,
    const ModelRef& model,
    int32_t camera_x,
    int32_t camera_z,
    bool first_frame
) {
    const formats::objects3d::Model& source = model.instance->model();
    const auto pieces = model.instance->pieces();
    for (std::size_t n = pieces.size(); n != 0; --n) {
        const PieceState& piece = pieces[n - 1];
        if ((piece.flags & piece_visible) != 0)
            draw_piece_flat(
                renderer,
                target,
                *model.unit,
                camera_x,
                camera_z,
                source.objects[piece.object_index],
                model.prepared->objects[piece.object_index],
                piece,
                model.unit->owner_index,
                first_frame
            );
    }
}

} // namespace

void draw_unit_model(
    ModelRenderer& renderer,
    Surface* target,
    const ModelRef& model,
    int32_t camera_x,
    int32_t camera_z,
    bool first_frame
) {
    unit_model_pass(
        renderer, target, model, camera_x, camera_z, first_frame, Pass::whole, nullptr, nullptr
    );
}

void plan_model_draw(ModelRenderer& renderer, const ModelRef& model, ModelDrawPlan& plan) {
    plan.from_image = has_image(*model.state);
    plan.ground = 0;
    plan.carried.clear();
    if (plan.from_image)
        unit_model_pass(renderer, nullptr, model, 0, 0, false, Pass::plan, &plan, nullptr);
}

void draw_planned_model(
    ModelRenderer& renderer,
    Surface* target,
    const ModelRef& model,
    int32_t camera_x,
    int32_t camera_z,
    bool first_frame,
    const ModelDrawPlan& plan
) {
    if (!plan.from_image) {
        draw_model_pieces(renderer, target, model, camera_x, camera_z, first_frame);
        return;
    }
    unit_model_pass(
        renderer, target, model, camera_x, camera_z, first_frame, Pass::draw, nullptr, &plan
    );
}

void build_model_image(
    ModelRenderer& renderer, Sprite& image, const ModelRef& model, uint8_t owner, int32_t pass
) {
    thread_local std::vector<present::DepthVertex> projected;
    thread_local std::vector<present::DepthVertex> corners;
    Sprite* target = begin_image_pass(renderer, image, model, pass);
    const bool doubled = target != &image;
    const formats::objects3d::Model& source = model.instance->model();
    const int32_t base = vertex_depth_base(model);
    const uint8_t team = renderer.team_colors[owner % 10];
    const auto pieces = model.instance->pieces();
    for (std::size_t n = pieces.size(); n != 0; --n) {
        const PieceState& piece = pieces[n - 1];
        if (!piece_passes(piece, pass, *model.unit))
            continue;
        project_image_vertices(piece, *target, doubled, base, renderer.samples, projected);
        const formats::objects3d::Object& object = source.objects[piece.object_index];
        const PreparedObject& prepared = model.prepared->objects[piece.object_index];
        for (uint32_t i = first_primitive(prepared); i < prepared.primitives.size(); ++i) {
            const PreparedPrimitive& primitive = prepared.primitives[i];
            if (!gather(source_of(object, primitive), projected, corners))
                continue;
            const auto count = static_cast<int32_t>(corners.size());
            if ((primitive.flags & primitive_colored) != 0) {
                present::fill_depth_polygon(*target, corners.data(), count, primitive.color);
            } else if (count == 4) {
                if (const Sprite* texture = primitive_texture(primitive, pass != 0, team);
                    texture != nullptr)
                    texture_depth_quad(target, texture, corners.data(), nullptr);
            }
        }
    }
    finish_image_pass(renderer, image, target);
}

void build_shaded_model_image(
    ModelRenderer& renderer, Sprite& image, const ModelRef& model, uint8_t owner, int32_t pass
) {
    struct Normal {
        float x{};
        float y{};
        float z{};
    };

    thread_local std::vector<present::DepthVertex> projected;
    thread_local std::vector<present::ShadedVertex> corners;
    thread_local std::vector<Normal> faces;
    thread_local std::vector<Normal> sums;
    thread_local std::vector<int32_t> uses;
    Sprite* target = begin_image_pass(renderer, image, model, pass);
    const bool doubled = target != &image;
    const formats::objects3d::Model& source = model.instance->model();
    const int32_t base = vertex_depth_base(model);
    const uint8_t team = renderer.team_colors[owner % 10];
    const auto pieces = model.instance->pieces();
    for (std::size_t n = pieces.size(); n != 0; --n) {
        const PieceState& piece = pieces[n - 1];
        if (!piece_passes(piece, pass, *model.unit))
            continue;
        const auto& points = piece.transformed_vertices;
        project_image_vertices(piece, *target, doubled, base, renderer.samples, projected);
        uses.assign(points.size(), 0);
        sums.assign(points.size(), Normal{0.0F, 0.0F, 0.0F});
        const formats::objects3d::Object& object = source.objects[piece.object_index];
        const PreparedObject& prepared = model.prepared->objects[piece.object_index];
        const uint32_t first = first_primitive(prepared);
        faces.assign(prepared.primitives.size(), Normal{0.0F, 1.0F, 0.0F});
        for (uint32_t i = first; i < prepared.primitives.size(); ++i) {
            const auto& indices = source_of(object, prepared.primitives[i]).vertex_indices;
            // Fewer than three corners, a repeated corner, or a corner past the
            // table keeps the straight-up normal.
            if (indices.size() < 3 || indices[0] == indices[1] || indices[1] == indices[2] ||
                indices[2] == indices[0] || indices[0] >= points.size() ||
                indices[1] >= points.size() || indices[2] >= points.size())
                continue;
            const FixedVector3& v0 = points[indices[0]];
            const FixedVector3& v1 = points[indices[1]];
            const FixedVector3& v2 = points[indices[2]];
            const base::geometry::Vec3f a =
                base::geometry::vec3f_int_delta(v1.x, v1.y, v1.z, v0.x, v0.y, v0.z);
            const base::geometry::Vec3f b =
                base::geometry::vec3f_int_delta(v1.x, v1.y, v1.z, v2.x, v2.y, v2.z);
            const Normal cross{
                static_cast<float>(static_cast<double>(b.z) * a.y - static_cast<double>(a.z) * b.y),
                static_cast<float>(static_cast<double>(a.z) * b.x - static_cast<double>(b.z) * a.x),
                static_cast<float>(static_cast<double>(b.y) * a.x - static_cast<double>(b.x) * a.y)
            };
            const double length = std::sqrt(
                static_cast<double>(cross.x) * cross.x + static_cast<double>(cross.y) * cross.y +
                static_cast<double>(cross.z) * cross.z
            );
            faces[i] = {
                static_cast<float>(cross.x / length),
                static_cast<float>(cross.y / length),
                static_cast<float>(cross.z / length)
            };
        }
        for (uint32_t i = first; i < prepared.primitives.size(); ++i) {
            for (const uint16_t index : source_of(object, prepared.primitives[i]).vertex_indices) {
                if (index >= points.size())
                    continue;
                ++uses[index];
                sums[index].x += faces[i].x;
                sums[index].y += faces[i].y;
                sums[index].z += faces[i].z;
            }
        }
        for (std::size_t v = 0; v < points.size(); ++v) {
            if (uses[v] == 0)
                continue;
            const auto count = static_cast<float>(uses[v]);
            sums[v] = {sums[v].x / count, sums[v].y / count, sums[v].z / count};
        }
        const bool lit = (piece.flags & piece_shaded) != 0;
        for (uint32_t i = first; i < prepared.primitives.size(); ++i) {
            const PreparedPrimitive& primitive = prepared.primitives[i];
            const auto& indices = source_of(object, primitive).vertex_indices;
            corners.clear();
            bool valid = true;
            for (const uint16_t index : indices) {
                if (index >= projected.size()) {
                    valid = false;
                    break;
                }
                int32_t shade = unlit_shade;
                if (lit) {
                    // z, y then x products summed, scaled by light_scale,
                    // truncated and masked to a shade row.
                    const Normal& normal = sums[index];
                    const double dot = (static_cast<double>(renderer.light[2]) * normal.z +
                                        static_cast<double>(renderer.light[1]) * normal.y) +
                                       static_cast<double>(renderer.light[0]) * normal.x;
                    shade = static_cast<int32_t>(
                        static_cast<uint32_t>(truncate_low32(dot * renderer.light_scale)) &
                        shade_row_mask
                    );
                }
                const present::DepthVertex& p = projected[index];
                corners.push_back({p.x, p.y, p.depth, shade});
            }
            if (!valid)
                continue;
            const auto count = static_cast<int32_t>(corners.size());
            if ((primitive.flags & primitive_colored) != 0) {
                present::fill_shaded_polygon(*target, corners.data(), count, primitive.color);
            } else if (count == 4) {
                if (const Sprite* texture = primitive_texture(primitive, pass != 0, team);
                    texture != nullptr)
                    texture_shaded_depth_quad(target, texture, corners.data(), nullptr);
            }
        }
    }
    finish_image_pass(renderer, image, target);
}

Sprite& copy_silhouette(ModelRenderer& renderer, const Sprite& image) {
    Sprite& composite = renderer.composite.sprite;
    ensure_composite(renderer, static_cast<int64_t>(image.width) * image.height);
    if (static_cast<int64_t>(plane_size(image)) > composite_plane(renderer))
        return composite;
    composite.width = image.width;
    composite.height = image.height;
    composite.origin_x = image.origin_x;
    composite.origin_y = image.origin_y;
    composite.key = image.key;
    std::memmove(composite.data, image.data, plane_size(image));
    if (image.aux != nullptr)
        std::memmove(composite.aux, image.aux, plane_size(image));
    present::clear_unkeyed_pixels(composite);
    return composite;
}

ImageFrame measure_shadow_bounds(const sim::model_runtime::Instance& instance) {
    return shadow_bounds_at(instance, 1);
}

void draw_shadow_silhouette(Sprite& target, const ModelRef& model) {
    shadow_silhouette_at(target, model, 1);
}

bool build_shadow_image(ModelRenderer& renderer, const ModelRef& model, const Sprite& image) {
    thread_local present::RleEncoder encoder;
    thread_local std::vector<uint8_t> stream;
    const uint32_t samples = renderer.samples;
    const ImageFrame frame = shadow_bounds_at(*model.instance, samples);
    ensure_composite(renderer, static_cast<int64_t>(frame.width) * frame.height);
    if (static_cast<int64_t>(frame.width) * frame.height > composite_plane(renderer))
        return false;
    Sprite& composite = renderer.composite.sprite;
    composite.width = static_cast<uint16_t>(frame.width);
    composite.height = static_cast<uint16_t>(frame.height);
    composite.origin_x = static_cast<int16_t>(frame.origin_x);
    composite.origin_y = static_cast<int16_t>(frame.origin_y);
    std::memset(
        composite.data, composite.key, static_cast<std::size_t>(frame.width) * frame.height
    );
    std::memset(composite.aux, 0, static_cast<std::size_t>(frame.width) * frame.height);
    shadow_silhouette_at(composite, model, samples);
    present::stamp_sprite_mask(
        image, composite, shadow_offset_x * static_cast<int32_t>(samples), 0
    );
    const int32_t size = present::encode_rle_sprite(encoder, nullptr, composite);
    stream.assign(static_cast<std::size_t>(size > 0 ? size : 0), 0);
    present::encode_rle_sprite(encoder, stream.data(), composite);
    present::SpriteBuffer& shadow = model.state->shadow;
    allocate_image(shadow, size, 1);
    if (shadow.sprite.data == nullptr)
        return false;
    std::memcpy(shadow.sprite.data, stream.data(), stream.size());
    shadow.sprite.width = composite.width;
    shadow.sprite.height = composite.height;
    shadow.sprite.origin_x = composite.origin_x;
    shadow.sprite.origin_y = composite.origin_y;
    shadow.sprite.encoding = OA_SPRITE_ROW_RLE;
    return true;
}

bool ground_shadow(
    uint16_t graphics_flags,
    const Game& game,
    const ModelRef& model,
    int32_t camera_x,
    int32_t camera_z,
    int32_t ground,
    GroundShadow& shadow
) {
    shadow.left = shadow.top = shadow.width = shadow.height = 0;
    shadow.coverage.clear();
    if (model.state == nullptr || model.unit == nullptr || !has_image(*model.state))
        return false;
    const ModelState& state = *model.state;
    const Unit& unit = *model.unit;
    const uint32_t flags = def_flags(model);
    if ((graphics_flags & graphics_shadows) == 0 || (flags & OA_UNIT_DEF_FLAG_NO_SHADOW) != 0)
        return false;
    // The choice draw_model makes, at one sample a game pixel.
    const bool vehicle_shadow =
        (graphics_flags & graphics_vehicle_shadows) != 0 &&
        (flags & (OA_UNIT_DEF_FLAG_FLOATER | OA_UNIT_DEF_FLAG_CAN_HOVER)) == 0;
    const int32_t unit_height = hi(unit.position.y);
    const bool submerged_standin =
        unit.type_index == 0 && static_cast<int16_t>(unit_height) < game.sea_level;
    const bool digger = (flags & OA_UNIT_DEF_FLAG_DIGGER) != 0;
    const bool building = is_building(unit);
    const Sprite& image = state.image.sprite;
    bool building_shadow = false;
    bool silhouette = false;
    // Depths at or below this are left out of the silhouette; -1 keeps all.
    int32_t clear_at_or_below = -1;
    if (image.aux == nullptr) {
        if (building && !digger)
            building_shadow = true;
        else
            silhouette = vehicle_shadow;
    } else if (digger) {
        silhouette = true;
        clear_at_or_below = static_cast<uint8_t>(vertex_depth_base(model));
    } else if (building) {
        building_shadow = true;
    } else if (vehicle_shadow) {
        silhouette = true;
        const int32_t lift = static_cast<int32_t>(game.sea_level) - unit_height;
        if (lift > 0)
            clear_at_or_below = static_cast<uint8_t>(vertex_depth_base(model) + lift);
    }
    if ((!building_shadow && !silhouette) || (building_shadow && submerged_standin))
        return false;
    const present::SpriteBuffer& buffer = building_shadow ? state.shadow : state.image;
    const Sprite& source = buffer.sprite;
    if (source.data == nullptr || source.width == 0 || source.height == 0)
        return false;
    const std::size_t count = static_cast<std::size_t>(source.width) * source.height;
    shadow.coverage.assign(count, 0);
    if (source.encoding == OA_SPRITE_RAW) {
        // The silhouette is every pixel of the image that is not its key.
        const auto* pixels = static_cast<const uint8_t*>(source.data);
        const auto* depth = static_cast<const uint8_t*>(source.aux);
        for (std::size_t i = 0; i < count; ++i)
            shadow.coverage[i] =
                pixels[i] != source.key && (clear_at_or_below < 0 || depth == nullptr ||
                                            depth[i] > static_cast<uint8_t>(clear_at_or_below));
    } else if (source.encoding == OA_SPRITE_ROW_RLE) {
        // A row-RLE stream draws every pixel its literals and runs write,
        // whatever their colour: decoded over two different fills, a pixel
        // either changed is one it writes.
        const std::span<const uint8_t> stream(buffer.pixels);
        const Rect32 rectangle{0, 0, source.width - 1, source.height - 1};
        thread_local std::vector<uint8_t> low;
        thread_local std::vector<uint8_t> high;
        low.assign(count, 0x00);
        high.assign(count, 0xff);
        if (!present::decode_rle_rows(low.data(), source.width, rectangle, stream, rectangle) ||
            !present::decode_rle_rows(high.data(), source.width, rectangle, stream, rectangle))
            return false;
        for (std::size_t i = 0; i < count; ++i)
            shadow.coverage[i] = low[i] != 0x00 || high[i] != 0xff;
    } else {
        return false;
    }
    shadow.left = hi(wrap_sub(unit.position.x, camera_x)) + shadow_offset_x - source.origin_x;
    shadow.top = hi(wrap_sub(unit.position.z, camera_z)) - (ground >> 1) - source.origin_y;
    shadow.width = source.width;
    shadow.height = source.height;
    return true;
}

void set_model_shift(const ModelRef& model, sim::model_runtime::RotationWords rotation) {
    ModelState& state = *model.state;
    const auto near = [](int16_t a, int16_t b) {
        return std::abs(static_cast<int16_t>(a - b)) < shift_threshold;
    };
    if (near(state.shift.yz, rotation.yz) && near(state.shift.xz, rotation.xz) &&
        near(state.shift.xy, rotation.xy))
        return;
    state.shift = rotation;
    state.transforms_dirty = true;
    auto pieces = model.instance->pieces();
    // Reset the piece that holds the model's root object; root_piece() finds
    // it after the pieces take the COB script's order.
    const uint32_t root = model.instance->root_piece();
    if (root >= pieces.size())
        return;
    pieces[root].transform_marker = 0;
    if ((pieces[root].flags & piece_cached) != 0)
        state.cache.draws = 0;
}

void update_model_transforms(const ModelRef& model) {
    const Unit& unit = *model.unit;
    set_model_shift(model, {unit.bank, static_cast<int16_t>(unit.heading), unit.pitch});
    // Either dirty flag rebuilds the transforms: a new root shift sets
    // transforms_dirty, and the piece setters raise model_runtime's own
    // dirty flag.
    if (model.state->transforms_dirty || model.instance->transforms_dirty()) {
        model.instance->rebuild_transforms(model.state->shift);
        model.state->transforms_dirty = false;
    }
}

void note_piece_changes(const ModelRef& model) {
    ModelState& state = *model.state;
    const auto pieces = model.instance->pieces();
    if (state.pieces.size() != pieces.size()) {
        state.pieces.resize(pieces.size());
        for (std::size_t i = 0; i < pieces.size(); ++i)
            state.pieces[i] = {pieces[i].translation, pieces[i].rotation, pieces[i].flags};
        return;
    }
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const PieceState& piece = pieces[i];
        ModelState::PieceSnapshot& seen = state.pieces[i];
        const bool moved =
            piece.translation.x != seen.translation.x ||
            piece.translation.y != seen.translation.y ||
            piece.translation.z != seen.translation.z || piece.rotation.xy != seen.rotation.xy ||
            piece.rotation.xz != seen.rotation.xz || piece.rotation.yz != seen.rotation.yz;
        const uint16_t changed = static_cast<uint16_t>(piece.flags ^ seen.flags);
        if ((moved || (changed & piece_visible) != 0) && (piece.flags & piece_cached) != 0)
            state.cache.draws = 0;
        if ((changed & piece_cached) != 0)
            state.image = {};
        seen = {piece.translation, piece.rotation, piece.flags};
    }
}

void update_linked_transforms(const ModelRenderer& renderer, const ModelRef& model) {
    const Unit& unit = *model.unit;
    update_model_transforms(model);
    for (const Unit* child = world_unit(renderer.world, unit.attach_first_child); child != nullptr;
         child = world_unit(renderer.world, child->attach_next)) {
        if ((child->flags & unit_flag_attached_without_piece) != 0 || renderer.model_of == nullptr)
            continue;
        const ModelRef carried = renderer.model_of(renderer.user, *child);
        if (carried.instance != nullptr)
            update_model_transforms(carried);
    }
}

LinkedDraw prepare_linked_draw(ModelRenderer& renderer, const ModelRef& model, bool movement_idle) {
    const Unit& unit = *model.unit;
    if (unit.attach_parent != 0)
        return {};
    update_linked_transforms(renderer, model);
    ModelState& state = *model.state;
    // The build finished since the image was built: the image goes, as the
    // build's end drops it.
    if (state.image_unfinished && unit.build_remaining == 0.0F) {
        state.image = {};
        state.image_unfinished = false;
    }
    state.cache.has_image = has_image(state);
    state.cache.image_has_mask = state.image.sprite.aux != nullptr;
    const UnitSpriteDraw draw = prepare_unit_sprite(unit, state.cache, movement_idle);
    if (!state.cache.has_image || draw.rebuild)
        state.shadow = {};
    if (draw.rebuild)
        prepare_model_image(renderer, model, false, pass_cached_pieces);
    state.cache.has_image = has_image(state);
    state.cache.image_has_mask = state.image.sprite.aux != nullptr;
    return {true, draw.unlit};
}

void draw_linked_model(
    ModelRenderer& renderer, Surface* target, const ModelRef& model, bool movement_idle
) {
    const LinkedDraw draw = prepare_linked_draw(renderer, model, movement_idle);
    if (!draw.drawn)
        return;
    draw_model(
        renderer,
        target,
        model,
        static_cast<int32_t>(static_cast<uint32_t>(renderer.camera_x) << 16),
        static_cast<int32_t>(static_cast<uint32_t>(renderer.camera_y) << 16),
        draw.unlit
    );
}

void draw_debris_piece(
    const ModelRenderer& renderer,
    Surface* target,
    const Rect32& view,
    const formats::objects3d::Object& object,
    const PreparedObject& prepared,
    const FixedVector3* points,
    const FixedVector3& origin,
    uint8_t team_color
) {
    thread_local std::vector<present::PolygonVertex> projected;
    const int32_t dx =
        wrap_sub(origin.x, static_cast<int32_t>(static_cast<uint32_t>(renderer.camera_x) << 16));
    const int32_t dz =
        wrap_sub(origin.z, static_cast<int32_t>(static_cast<uint32_t>(renderer.camera_y) << 16));
    const int32_t x = hi(dx) + renderer.origin_x;
    const int32_t y = hi(dz) - (hi(origin.y) >> 1) + renderer.origin_y;
    if (x < view.x1 || view.x2 < x || y < view.y1 || view.y2 < y)
        return;
    projected.clear();
    for (std::size_t i = 0; i < object.vertices.size(); ++i) {
        const FixedVector3& p = points[i];
        projected.push_back(
            {hi(wrap_add(p.x, dx)) + renderer.origin_x,
             (hi(wrap_sub(dz, p.z)) - (hi(wrap_add(p.y, origin.y)) >> 1)) + renderer.origin_y}
        );
    }
    draw_flat_primitives(
        target, object, prepared, projected, [&](const PreparedPrimitive& primitive) {
            return primitive_texture(primitive, false, team_color);
        }
    );
}

void draw_rotated_debris(
    const ModelRenderer& renderer,
    Surface* target,
    const Rect32& view,
    const formats::objects3d::Object& object,
    const PreparedObject& prepared,
    sim::model_runtime::RotationWords rotation,
    const FixedVector3& origin,
    uint8_t team_color,
    std::vector<FixedVector3>& points
) {
    points.resize(object.vertices.size());
    for (std::size_t n = object.vertices.size(); n != 0; --n) {
        const FixedVector3& v = object.vertices[n - 1];
        points[n - 1] =
            sim::model_runtime::rotate_vector({wrap_sub(0, v.x), v.y, wrap_sub(0, v.z)}, rotation);
    }
    draw_debris_piece(renderer, target, view, object, prepared, points.data(), origin, team_color);
}

void draw_rotated_object(
    const ModelRenderer& renderer,
    Surface* target,
    const FixedVector3& position,
    const FixedVector3* points,
    const formats::objects3d::Object& object,
    const PreparedObject& prepared,
    sim::model_runtime::RotationWords rotation
) {
    thread_local std::vector<present::PolygonVertex> projected;
    projected.clear();
    for (std::size_t i = 0; i < object.vertices.size(); ++i) {
        const FixedVector3 p = sim::model_runtime::rotate_vector(points[i], rotation);
        projected.push_back(
            {hi(wrap_add(p.x, position.x)) + renderer.origin_x,
             (hi(wrap_sub(position.z, p.z)) - (hi(wrap_add(p.y, position.y)) >> 1)) +
                 renderer.origin_y}
        );
    }
    draw_flat_primitives(target, object, prepared, projected, cursor_texture);
}

void draw_projectile_model(
    const ModelRenderer& renderer,
    Surface* target,
    const FixedVector3& position,
    const formats::objects3d::Object& object,
    const PreparedObject& prepared,
    sim::model_runtime::RotationWords rotation
) {
    thread_local std::vector<FixedVector3> points;
    points.clear();
    for (const FixedVector3& v : object.vertices)
        points.push_back({wrap_sub(0, v.x), v.y, wrap_sub(0, v.z)});
    draw_rotated_object(renderer, target, position, points.data(), object, prepared, rotation);
}

void draw_shatter_fragment(
    const ModelRenderer& renderer,
    Surface* target,
    const FixedVector3& position,
    const sim::effect_particles::ShatterFragment& fragment,
    const PreparedPrimitive& source,
    sim::model_runtime::RotationWords spin
) {
    namespace fx = sim::effect_particles;
    // The arena's object record: eight points, six faces over the game's
    // fragment corner table, no selection primitive.
    static const formats::objects3d::Object slab = [] {
        formats::objects3d::Object object;
        object.vertices.resize(fx::fragment_point_count);
        for (const auto& corners : fx::fragment_faces) {
            formats::objects3d::Primitive face;
            face.vertex_indices.assign(std::begin(corners), std::end(corners));
            object.primitives.push_back(face);
        }
        return object;
    }();
    PreparedPrimitive look = source;
    look.flags = fragment.look.flags;
    look.color = static_cast<uint8_t>(fragment.look.color);
    constexpr uint32_t shown_bits = primitive_colored | primitive_animated | primitive_team;
    if ((look.flags & shown_bits) == primitive_team && (source.flags & primitive_animated) != 0)
        look.frame = primitive_texture(source, false, fragment.look.team_color);
    thread_local PreparedObject faces;
    faces.skips_first = false;
    faces.primitives.assign(fx::fragment_face_count, look);
    for (uint32_t i = 0; i < fx::fragment_face_count; ++i)
        faces.primitives[i].source_index = i;
    FixedVector3 points[fx::fragment_point_count];
    for (uint32_t i = 0; i < fx::fragment_point_count; ++i)
        points[i] = {fragment.points[i].x, fragment.points[i].y, fragment.points[i].z};
    draw_rotated_object(renderer, target, position, points, slab, faces, spin);
}

} // namespace oa::present::model
