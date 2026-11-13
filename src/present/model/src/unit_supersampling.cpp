// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/model/unit_supersampling.hpp"

#include <algorithm>
#include <array>
#include <memory>

namespace oa::present::model {
namespace {

// The levels from the finest down.
constexpr std::array<UnitSupersampling, 5> levels_down{
    UnitSupersampling::x16,
    UnitSupersampling::x8,
    UnitSupersampling::x4,
    UnitSupersampling::x2,
    UnitSupersampling::off,
};

// The finer draw state of a model's draw state, made when first asked for.
FinerModel& finer_model(ModelState& state) {
    if (!state.finer)
        state.finer = std::make_unique<FinerModel>();
    return *state.finer;
}

// The renderer's own callbacks and their context, which a finer draw passes
// its callbacks through.
struct FinerLinks {
    void* user{};
    int32_t (*ground_height)(void* user, const FixedVec3& position){};
    ModelRef (*model_of)(void* user, const Unit& unit){};
};

// Returns the terrain height through the renderer's own callback.
int32_t finer_ground_height(void* user, const FixedVec3& position) {
    const auto& links = *static_cast<const FinerLinks*>(user);
    return links.ground_height(links.user, position);
}

// Returns a carried unit's model with its finer draw state in place of its
// own, so that a finer draw builds the carried unit's image there and leaves
// the game's image alone. That image holds the unit as carried, so the
// unit's own finer draws build theirs again.
ModelRef finer_model_of(void* user, const Unit& unit) {
    const auto& links = *static_cast<const FinerLinks*>(user);
    ModelRef model = links.model_of(links.user, unit);
    if (model.state != nullptr) {
        FinerModel& finer = finer_model(*model.state);
        finer.samples = 0;
        model.state = &finer.state;
    }
    return model;
}

// Sets a renderer up to draw finer over a sampled region, and puts it back
// as it was when it goes.
class FinerRenderer {
  public:

    FinerRenderer(ModelRenderer& renderer, uint32_t factor, const Rect32& region)
        : renderer_(renderer), links_{renderer.user, renderer.ground_height, renderer.model_of},
          samples_(renderer.samples), origin_x_(renderer.origin_x), origin_y_(renderer.origin_y) {
        const auto samples = static_cast<int32_t>(factor);
        renderer.samples = factor;
        renderer.origin_x = (origin_x_ - region.x1) * samples;
        renderer.origin_y = (origin_y_ - region.y1) * samples;
        renderer.user = &links_;
        renderer.ground_height = links_.ground_height != nullptr ? finer_ground_height : nullptr;
        renderer.model_of = links_.model_of != nullptr ? finer_model_of : nullptr;
    }

    FinerRenderer(const FinerRenderer&) = delete;
    FinerRenderer& operator=(const FinerRenderer&) = delete;

    ~FinerRenderer() {
        renderer_.samples = samples_;
        renderer_.origin_x = origin_x_;
        renderer_.origin_y = origin_y_;
        renderer_.user = links_.user;
        renderer_.ground_height = links_.ground_height;
        renderer_.model_of = links_.model_of;
    }

  private:

    ModelRenderer& renderer_;
    FinerLinks links_{};
    uint32_t samples_{};
    int32_t origin_x_{};
    int32_t origin_y_{};
};

// Game pixels added around the region a unit's draw covers, for the
// rounding of places drawn finer.
constexpr int32_t drawn_region_margin = 2;

// Grows a region to hold a rectangle.
void include_rect(Rect32& region, int32_t x, int32_t y, int32_t width, int32_t height) noexcept {
    region.x1 = std::min(region.x1, x);
    region.y1 = std::min(region.y1, y);
    region.x2 = std::max(region.x2, x + width);
    region.y2 = std::max(region.y2, y + height);
}

// The region of the bridge, in its 8-bit pixels, a unit's draw covers: its
// model, its cached image and building silhouette, its ground shadow and the
// units it carries, from its current transforms, with drawn_region_margin
// around them.
Rect32 drawn_region(const ModelRenderer& renderer, const ModelRef& model) {
    const Unit& unit = *model.unit;
    const auto high = [](int32_t value) { return value >> 16; };
    const auto camera = [](int32_t pixels) {
        return static_cast<int32_t>(static_cast<uint32_t>(pixels) << 16);
    };
    const int32_t x = high(
                          static_cast<int32_t>(
                              static_cast<uint32_t>(unit.position.x) -
                              static_cast<uint32_t>(camera(renderer.camera_x))
                          )
                      ) +
                      renderer.origin_x;
    const int32_t z = high(
                          static_cast<int32_t>(
                              static_cast<uint32_t>(unit.position.z) -
                              static_cast<uint32_t>(camera(renderer.camera_y))
                          )
                      ) +
                      renderer.origin_y;
    const int32_t unit_y = z - (high(unit.position.y) >> 1);
    const int32_t ground = renderer.ground_height != nullptr
                               ? renderer.ground_height(renderer.user, unit.position)
                               : 0;
    const int32_t shadow_x = x + shadow_offset_x;
    const int32_t shadow_y = z - (ground >> 1);
    Rect32 region{x, unit_y, x, unit_y};
    const ImageFrame frame = measure_model_bounds(*model.instance, nullptr);
    include_rect(region, x - frame.origin_x, unit_y - frame.origin_y, frame.width, frame.height);
    include_rect(
        region, shadow_x - frame.origin_x, shadow_y - frame.origin_y, frame.width, frame.height
    );
    const ImageFrame shear = measure_shadow_bounds(*model.instance);
    include_rect(
        region, shadow_x - shear.origin_x, shadow_y - shear.origin_y, shear.width, shear.height
    );
    for (const Sprite* sprite : {&model.state->image.sprite, &model.state->shadow.sprite}) {
        if (sprite->data == nullptr)
            continue;
        include_rect(
            region, x - sprite->origin_x, unit_y - sprite->origin_y, sprite->width, sprite->height
        );
        include_rect(
            region,
            shadow_x - sprite->origin_x,
            shadow_y - sprite->origin_y,
            sprite->width,
            sprite->height
        );
    }
    ModelBounds carried{};
    bool carries = false;
    for (const Unit* child = world_unit(renderer.world, unit.attach_first_child); child != nullptr;
         child = world_unit(renderer.world, child->attach_next)) {
        if (renderer.model_of == nullptr)
            break;
        const ModelRef cargo = renderer.model_of(renderer.user, *child);
        if (cargo.instance == nullptr)
            continue;
        expand_model_bounds(
            carried,
            *cargo.instance,
            static_cast<int32_t>(
                static_cast<uint32_t>(cargo.unit->position.x) -
                static_cast<uint32_t>(unit.position.x)
            ),
            static_cast<int32_t>(
                static_cast<uint32_t>(cargo.unit->position.y) -
                static_cast<uint32_t>(unit.position.y)
            ),
            static_cast<int32_t>(
                static_cast<uint32_t>(cargo.unit->position.z) -
                static_cast<uint32_t>(unit.position.z)
            )
        );
        carries = true;
    }
    if (carries)
        include_rect(
            region,
            x + carried.left,
            unit_y + carried.top,
            carried.right - carried.left,
            carried.bottom - carried.top
        );
    return {
        region.x1 - drawn_region_margin,
        region.y1 - drawn_region_margin,
        region.x2 + drawn_region_margin,
        region.y2 + drawn_region_margin
    };
}

// A region clipped to a surface. Rect32 is packed, so its fields are read
// into locals before std::max and std::min take them by reference.
Rect32 clip_to_surface(const Rect32& region, const Surface& surface) noexcept {
    const int32_t left = region.x1;
    const int32_t top = region.y1;
    const int32_t right = region.x2;
    const int32_t bottom = region.y2;
    return {
        std::max(left, 0),
        std::max(top, 0),
        std::min(right, surface.width - 1),
        std::min(bottom, surface.height - 1)
    };
}

// The overlap of two regions, read field by field as clip_to_surface reads them.
Rect32 overlap(const Rect32& a, const Rect32& b) noexcept {
    const int32_t a_left = a.x1;
    const int32_t a_top = a.y1;
    const int32_t a_right = a.x2;
    const int32_t a_bottom = a.y2;
    const int32_t b_left = b.x1;
    const int32_t b_top = b.y1;
    const int32_t b_right = b.x2;
    const int32_t b_bottom = b.y2;
    return {
        std::max(a_left, b_left),
        std::max(a_top, b_top),
        std::min(a_right, b_right),
        std::min(a_bottom, b_bottom)
    };
}

// The region of the bridge's surface a sampled region covers, as
// bridge_open_sampled lays it out: the region clipped to the surface, or an
// empty one.
Rect32 sampled_region(const RgbBridge& bridge, const Rect32& region) noexcept {
    const Rect32 clip = clip_to_surface(region, bridge.surface);
    if (clip.x1 > clip.x2 || clip.y1 > clip.y2)
        return {0, 0, -1, -1};
    return clip;
}

/// Tells whether a region can change any pixel of the drawing band's surface.
bool region_reaches_band(
    const RgbBridge& bridge, const BridgeBand* band, const Rect32& region
) noexcept {
    if (band == nullptr)
        return true;
    const Rect32 clip = clip_to_surface(region, bridge.surface);
    return clip.x1 <= clip.x2 && clip.y1 <= clip.y2 && clip.y1 < band->end_row &&
           clip.y2 >= band->first_row;
}

} // namespace

std::optional<UnitSupersampling> unit_supersampling_from_factor(uint32_t factor) noexcept {
    switch (factor) {
    case 1:
        return UnitSupersampling::off;
    case 2:
        return UnitSupersampling::x2;
    case 4:
        return UnitSupersampling::x4;
    case 8:
        return UnitSupersampling::x8;
    case 16:
        return UnitSupersampling::x16;
    default:
        return std::nullopt;
    }
}

UnitSupersampling fitting_supersampling(const Rect32& region, UnitSupersampling level) noexcept {
    if (region.x1 > region.x2 || region.y1 > region.y2)
        return UnitSupersampling::off;
    const auto pixels = static_cast<uint64_t>(region.x2 - region.x1 + 1) *
                        static_cast<uint64_t>(region.y2 - region.y1 + 1);
    for (const UnitSupersampling candidate : levels_down) {
        const uint64_t factor = supersampling_factor(candidate);
        if (factor <= supersampling_factor(level) &&
            pixels * factor * factor <= supersample_max_samples)
            return candidate;
    }
    return UnitSupersampling::off;
}

void plan_unit_supersampled(
    ModelRenderer& renderer,
    const RgbBridge& bridge,
    const ModelRef& model,
    const Rect32& region,
    bool movement_idle,
    UnitSupersampling level,
    SupersampledUnitPlan& plan,
    ModelImagePlan image_plan,
    ModelShadowSource shadow_source
) {
    const Rect32 clipped = clip_to_surface(region, bridge.surface);
    const UnitSupersampling fitting =
        level == UnitSupersampling::off ? level : fitting_supersampling(clipped, level);
    plan.level = fitting;
    plan.region = region;
    plan.covered = {0, 0, -1, -1};
    plan.finer = {};
    plan.model.from_image = false;
    plan.model.carried.clear();
    if (fitting == UnitSupersampling::off) {
        // As draw_linked_model readies and builds, once the region is
        // captured; any finer image the unit kept is let go.
        model.state->finer.reset();
        if ((renderer.graphics_flags & graphics_shadows) != 0 &&
            shadow_source != ModelShadowSource::verified_geometry)
            image_plan = ModelImagePlan::pixels;
        plan.linked = prepare_linked_draw(renderer, model, movement_idle, image_plan);
        if (plan.linked.drawn)
            plan_model_draw(renderer, model, plan.model, image_plan);
        return;
    }
    plan.linked = prepare_linked_draw(renderer, model, movement_idle);
    if (!plan.linked.drawn)
        return;
    // The unit's transforms are now those it draws with, so its draw covers
    // no more than drawn_region, which is far smaller than the region the
    // caller allows from the last transforms.
    plan.covered = overlap(clipped, drawn_region(renderer, model));
    ModelState& state = *model.state;
    FinerModel& finer = finer_model(state);
    ModelRef finer_ref = model;
    finer_ref.state = &finer.state;
    const uint32_t factor = supersampling_factor(fitting);
    {
        const FinerRenderer finer_renderer(renderer, factor, sampled_region(bridge, plan.covered));
        if (state.image.sprite.data == nullptr) {
            finer.state.image = {};
            finer.state.shadow = {};
        } else if (
            finer.samples != factor || finer.image_builds != state.image_builds ||
            finer.state.image.sprite.data == nullptr
        ) {
            finer.state.shadow = {};
            prepare_model_image(renderer, finer_ref, false, pass_cached_pieces);
            finer.samples = factor;
            finer.image_builds = state.image_builds;
        }
        plan_model_draw(renderer, finer_ref, plan.model);
    }
    plan.finer = finer_ref;
}

void draw_planned_unit(
    ModelRenderer& renderer,
    RgbBridge& bridge,
    BridgeBand* band,
    SupersampleScratch& scratch,
    const ModelRef& model,
    const SupersampledUnitPlan& plan,
    bool& bridge_holds_draws
) {
    const auto camera_x = static_cast<int32_t>(static_cast<uint32_t>(renderer.camera_x) << 16);
    const auto camera_z = static_cast<int32_t>(static_cast<uint32_t>(renderer.camera_y) << 16);
    if (plan.level == UnitSupersampling::off) {
        // Into the bridge as it always was, to be written back with its
        // other draws.
        if (band != nullptr)
            bridge_open(bridge, *band, plan.region);
        else
            bridge_open(bridge, plan.region);
        if (plan.linked.drawn && region_reaches_band(bridge, band, plan.region))
            draw_planned_model(
                renderer,
                band != nullptr ? &band->surface : &bridge.surface,
                model,
                camera_x,
                camera_z,
                plan.linked.unlit,
                plan.model
            );
        bridge_holds_draws = true;
        return;
    }
    if (!plan.linked.drawn)
        return;
    // Everything the bridge holds goes to the frame first, so that the unit
    // draws over it: the draws it notes, and lines drawn into the bridge
    // without being noted, such as the unit's own selection box.
    if (band != nullptr)
        bridge_end(bridge, *band);
    else
        bridge_end(bridge);
    bridge_holds_draws = false;
    // A sampled draw still flushes earlier bridge pixels in this band,
    // even when only another band contains the unit itself.
    if (!region_reaches_band(bridge, band, plan.covered))
        return;
    SampledRegion& sampled = scratch.region;
    const uint32_t factor = supersampling_factor(plan.level);
    if (band != nullptr)
        bridge_open_sampled(bridge, *band, sampled, plan.covered, factor);
    else
        bridge_open_sampled(bridge, sampled, plan.covered, factor);
    {
        const FinerRenderer finer_renderer(renderer, sampled.factor, sampled.region);
        draw_planned_model(
            renderer,
            &sampled.surface,
            plan.finer,
            camera_x,
            camera_z,
            plan.linked.unlit,
            plan.model
        );
    }
    if (band != nullptr)
        bridge_end_sampled(bridge, *band, sampled);
    else
        bridge_end_sampled(bridge, sampled);
}

void draw_unit_supersampled(
    ModelRenderer& renderer,
    RgbBridge& bridge,
    SupersampleScratch& scratch,
    const ModelRef& model,
    const Rect32& region,
    bool movement_idle,
    UnitSupersampling level,
    bool& bridge_holds_draws
) {
    SupersampledUnitPlan plan;
    plan_unit_supersampled(renderer, bridge, model, region, movement_idle, level, plan);
    draw_planned_unit(renderer, bridge, nullptr, scratch, model, plan, bridge_holds_draws);
}

} // namespace oa::present::model
