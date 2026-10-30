// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Composition of the match battlefield frame, and the groups of text it
// lays over the backings painted with them (BackedText).
#include "oa/app/runtime.hpp"
#include "gui_actions.hpp"
#include "oa/app/far_view.hpp"
#include "oa/ui/hud/command_buttons.hpp"
#include "director_state.hpp"
#include "match_models.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/match_model_draws.hpp"
#include "presentation_interpolation.hpp"
#include "render_run.hpp"
#include "render_state.hpp"
#include "world_draws.hpp"
#include "renderer_weapon_style.hpp"
#include "xrgb_conversion.hpp"
#include "oa/base/float_precision.hpp"
#include "oa/present/model/model_draw.hpp"
#include "oa/present/model/rgb_bridge.hpp"
#include "oa/present/blit.hpp"
#include "oa/present/display.hpp"
#include "oa/present/game_text.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/scene_grid.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/hud/backed_text.hpp"
#include "oa/ui/hud/health_bar.hpp"
#include "oa/ui/hud/shared_views.hpp"
#include "oa/present/model/sprite_placement.hpp"
#include "oa/present/world_renderer/scene_filter.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/present/world_renderer/world_draw_order.hpp"
#include "oa/present/world_renderer/world_overlays.hpp"
#include "oa/ui/decoded.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oa::app {

static_assert(
    kMinFullBattlefieldZoom == oa::ui::hud::kHealthBarFurthestZoom,
    "the health bars reach their smallest at the furthest zoom out"
);

namespace {

namespace model_render = oa::present::model;
namespace unit_playout = oa::present::unit_playout;

// Margin around the estimated screen extent of a model draw.
constexpr int32_t kModelRegionMargin = 16;
// Reach of a projectile model around its screen point.
constexpr int32_t kProjectileReach = 64;
// Weapon render types drawn as 3DO models, and the half turn added to the
// missile's heading and pitch words.
constexpr uint8_t kRenderMissile = 1;
constexpr uint8_t kRenderBomb = 3;
constexpr uint8_t kRenderModel = 6;
constexpr uint16_t kHalfTurn = 0x8000;
// Texture animation steps replayed at most per frame.
constexpr uint32_t kMaxAnimationSteps = 30;

int32_t high_word(int32_t value) {
    return static_cast<int16_t>(static_cast<uint32_t>(value) >> 16);
}

int32_t camera_fixed(int32_t pixels) {
    return static_cast<int32_t>(static_cast<uint32_t>(pixels) << 16);
}

int32_t wrapping_sub(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

int32_t terrain_height(void* user, const oa::FixedVec3& position) {
    auto* models = static_cast<MatchModels*>(user);
    return models->offline->map_height(
        static_cast<uint32_t>(position.x), static_cast<uint32_t>(position.z)
    );
}

/// Returns what drawing needs of a unit's model: its instance, prepared model, draw state and records.
///
/// The draw state is the slot's for its current instance generation, so a
/// unit made where another died draws from fresh state.
///
/// @param[in,out] models the match's renderer state; its slot's draw state
///     begins afresh for a new instance
/// @param slot unit slot
/// @return the model, or a null instance for slot 0, a slot outside the pool
///     or one without an instance
model_render::ModelRef unit_model(MatchModels& models, uint16_t slot) {
    auto& world = models.offline->world();
    if (slot == 0 || slot >= world.record.unit_slot_count)
        return {};
    auto& runtime = models.offline->runtime_state(slot);
    if (!runtime.instance)
        return {};
    auto& instance = runtime.instance->model();
    auto& state = unit_draw_state(models.units, slot, runtime.instance_generation);
    const oa::Unit& record = world.record.units[slot];
    return {
        &instance,
        &model_render::prepare_model(models.library, instance.model_handle()),
        &state,
        &record,
        oa::world_unit_def_of(&world.record, &record)
    };
}

/// Grows the unit slots' draw states (MatchModels::units) to hold every slot
/// with a model instance, as unit_model would grow them slot by slot.
///
/// @param[in,out] models the match's renderer state
void hold_unit_draw_states(MatchModels& models) {
    const uint32_t slots = models.offline->world().record.unit_slot_count;
    uint32_t highest = 0;
    for (uint32_t slot = 1; slot < slots; ++slot)
        if (models.offline->runtime_state(static_cast<uint16_t>(slot)).instance)
            highest = slot;
    if (models.units.size() <= highest)
        models.units.resize(static_cast<std::size_t>(highest) + 1);
}

/// Returns the prepared model of a unit type's model known only by address.
///
/// Debris, shatter fragments and the shatter loader carry a unit's model by
/// address; the loaded unit type the unit was made from owns it. An entry the
/// library holds for the live model is returned; otherwise the model is
/// prepared through that type's handle.
///
/// @param[in,out] library the match's model library
/// @param types the match's loaded unit types
/// @param model a live model
/// @return the prepared model, or null when no loaded type owns the model
const model_render::PreparedModel* prepared_type_model(
    model_render::ModelLibrary& library,
    std::span<const oa::sim::unit_spawn::LoadedType> types,
    const oa::formats::objects3d::Model& model
) {
    if (const auto* prepared = model_render::find_prepared_model(library, model))
        return prepared;
    for (const auto& type : types)
        if (type.model.get() == &model)
            return &model_render::prepare_model(library, type.model);
    return nullptr;
}

/// Tells whether a draw between ticks shows a unit slot otherwise than its
/// tick does: the unit moved, turned or moved a piece since the tick before.
///
/// @param models the match's renderer state, its tick noted
/// @param slot unit slot
/// @return true when the slot's unit moved
bool unit_moved(const MatchModels& models, uint32_t slot) {
    const auto& units = models.presentation.units;
    return slot < units.size() && units[slot].seen && units[slot].moved;
}

/// Tells whether a unit or a unit it carries moved since the tick before.
///
/// @param models the match's renderer state, its tick noted
/// @param world the match's World
/// @param slot the unit's slot
/// @return true when the unit or one it carries moved
bool unit_or_cargo_moved(const MatchModels& models, const oa::World& world, uint16_t slot) {
    if (unit_moved(models, slot))
        return true;
    const oa::Unit* unit = oa::world_unit_at(&world, slot);
    if (unit == nullptr)
        return false;
    for (const oa::Unit* child = oa::world_unit(&world, unit->attach_first_child); child != nullptr;
         child = oa::world_unit(&world, child->attach_next))
        if (unit_moved(models, oa::world_unit_slot(&world, child)))
            return true;
    return false;
}

/// Returns the copies a unit draws from part of the way through the current
/// tick: its record and model instance, placed between its two poses when it
/// moved, with a draw state of their own. A unit of another machine's player
/// is placed, turned and tilted on its playout (mirrored_pose), whole ticks
/// included, and its pieces are placed between their two poses even when its
/// records moved it by a jump. The match's record, instance and draw state
/// are only read.
///
/// The copies are placed once a draw; a later call of the same draw returns
/// them as they are.
///
/// @param[in,out] models the match's renderer state, its tick noted
/// @param slot unit slot of a live unit with a model instance
/// @return the copies' model
model_render::ModelRef presented_unit(MatchModels& models, uint16_t slot) {
    auto& world = models.offline->world();
    auto& presentation = models.presentation;
    auto& runtime = models.offline->runtime_state(slot);
    if (presentation.units.size() <= slot || !runtime.instance)
        return unit_model(models, slot);
    auto& motion = presentation.units[slot];
    const oa::Unit& record = world.record.units[slot];
    if (motion.blended_draw != presentation.draw) {
        motion.record = record;
        motion.instance = runtime.instance->model();
        const auto pose = mirrored_pose(
            models, world.record, slot, playout_moment(presentation, presentation.fraction)
        );
        if (motion.moved)
            blend_unit_pose(
                motion.previous,
                motion.current,
                presentation.fraction,
                motion.record,
                motion.instance,
                presentation.batch
            );
        else if (pose && motion.pieces_moved)
            // Placed on its playout, its pieces move between their ticks
            // however far its records moved it.
            blend_unit_pieces(
                motion.previous,
                motion.current,
                presentation.fraction,
                motion.instance,
                presentation.batch
            );
        if (pose) {
            motion.record.position = pose->position;
            motion.record.heading = pose->heading;
            motion.record.pitch = pose->pitch;
            motion.record.bank = pose->bank;
        }
        // The pieces were set without the setters, which would mark the
        // transforms for rebuilding.
        motion.state.transforms_dirty = true;
        motion.blended_draw = presentation.draw;
    }
    return {
        &motion.instance,
        &model_render::prepare_model(models.library, motion.instance.model_handle()),
        &motion.state,
        &motion.record,
        oa::world_unit_def_of(&world.record, &record)
    };
}

model_render::ModelRef carried_model(void* user, const oa::Unit& unit) {
    auto* models = static_cast<MatchModels*>(user);
    const auto& world = models->offline->world().record;
    const auto slot = static_cast<uint16_t>(oa::world_unit_slot(&world, &unit));
    // A carrier drawn from its copies carries its units' copies.
    if (models->presentation.blending)
        return presented_unit(*models, slot);
    return unit_model(*models, slot);
}

void include(oa::Rect32& region, int32_t x, int32_t y, int32_t width, int32_t height) {
    const int32_t left = region.x1;
    const int32_t top = region.y1;
    const int32_t right = region.x2;
    const int32_t bottom = region.y2;
    region.x1 = std::min(left, x);
    region.y1 = std::min(top, y);
    region.x2 = std::max(right, x + width);
    region.y2 = std::max(bottom, y + height);
}

// Bridge region a unit draw can touch: its model, cached image and ground
// shadow, from the last transforms plus a margin.
oa::Rect32 unit_region(
    const model_render::ModelRenderer& renderer, const model_render::ModelRef& model, int32_t ground
) {
    const oa::Unit& unit = *model.unit;
    const int32_t x = high_word(wrapping_sub(unit.position.x, camera_fixed(renderer.camera_x))) +
                      renderer.origin_x;
    const int32_t z = high_word(wrapping_sub(unit.position.z, camera_fixed(renderer.camera_y))) +
                      renderer.origin_y;
    const int32_t unit_y = z - (high_word(unit.position.y) >> 1);
    const int32_t shadow_x = x + model_render::shadow_offset_x;
    const int32_t shadow_y = z - (ground >> 1);
    oa::Rect32 region{x, unit_y, x, unit_y};
    const auto frame = model_render::measure_model_bounds(*model.instance, nullptr);
    include(region, x - frame.origin_x, unit_y - frame.origin_y, frame.width, frame.height);
    include(
        region, shadow_x - frame.origin_x, shadow_y - frame.origin_y, frame.width, frame.height
    );
    const auto shear = model_render::measure_shadow_bounds(*model.instance);
    include(
        region, shadow_x - shear.origin_x, shadow_y - shear.origin_y, shear.width, shear.height
    );
    for (const oa::Sprite* sprite : {&model.state->image.sprite, &model.state->shadow.sprite}) {
        if (sprite->data == nullptr)
            continue;
        include(
            region, x - sprite->origin_x, unit_y - sprite->origin_y, sprite->width, sprite->height
        );
        include(
            region,
            shadow_x - sprite->origin_x,
            shadow_y - sprite->origin_y,
            sprite->width,
            sprite->height
        );
    }
    return {
        region.x1 - kModelRegionMargin,
        region.y1 - kModelRegionMargin,
        region.x2 + kModelRegionMargin,
        region.y2 + kModelRegionMargin
    };
}

/// Returns the part of a bridge region over the map the view shows.
///
/// The bridge's pixels are the map pixels from the renderer's camera on,
/// one for one, so nothing drawn within the part lies past the map's edges.
///
/// @param region the region, inclusive bridge pixels
/// @param renderer the renderer, whose camera and origin place the map
/// @param map_width map pixels across the map the view shows
/// @param map_height map pixels down
/// @return the part over the map; a region off the map is left with its
///         right edge before its left, which captures nothing
oa::Rect32 region_on_map(
    const oa::Rect32& region,
    const model_render::ModelRenderer& renderer,
    int32_t map_width,
    int32_t map_height
) {
    const int32_t left = region.x1;
    const int32_t top = region.y1;
    const int32_t right = region.x2;
    const int32_t bottom = region.y2;
    const int32_t map_left = renderer.origin_x - renderer.camera_x;
    const int32_t map_top = renderer.origin_y - renderer.camera_y;
    return {
        std::max(left, map_left),
        std::max(top, map_top),
        std::min(right, map_left + map_width - 1),
        std::min(bottom, map_top + map_height - 1)
    };
}

/// Tells whether a unit's owner is another machine's player: in use with
/// OA_PLAYER_STATUS_MIRRORED. Only such a player's records move its units;
/// the units of a player in use but free or closed stay where the
/// simulation has them.
///
/// @param world the match's World
/// @param unit the unit
/// @return true for a unit of a mirrored player
bool mirrored_owner(const oa::World& world, const oa::Unit& unit) noexcept {
    if (unit.owner_index >= OA_PLAYER_COUNT)
        return false;
    const oa::Player& owner = world.game.players[unit.owner_index];
    return owner.in_use != 0 && owner.status == OA_PLAYER_STATUS_MIRRORED;
}

/// Draws the frame's battlefield draws (MatchModels::draws) band by band.
///
/// The bridge is split into one band for each thread the pool runs a job
/// on (bridge_split), and the bands draw at once on the pool, or the one
/// band covering the frame draws on the calling thread alone, as it does
/// whenever the frame holds a projectile's lens, which reads the rows of
/// other bands (draw_world_lens). The first band
/// draws with the models' renderer and buffers; each other band with its
/// own (MatchModels::band_scratch), the frame's renderer settings copied in,
/// and its own memory of colours outside the palette. A band that fails
/// lets the others finish; the failure of the first band in band order that
/// failed is then thrown on the calling thread.
///
/// @param[in,out] models the match's renderer state, its draws worked out
/// @param frame what every band draws with
/// @param pool the drawing threads; null draws on the calling thread alone
void draw_world_bands(
    MatchModels& models, const WorldFrameDraw& frame, oa::platform::job_pool::Pool* pool
) {
    const auto threads =
        pool != nullptr && models.draws.lenses.empty()
            ? static_cast<int32_t>(std::min(pool->threads(), oa::platform::job_pool::max_threads))
            : 1;
    const int32_t made = model_render::bridge_split(models.bridge, threads, models.bands);
    models.most_bands = std::max(models.most_bands, made);
    if (models.band_scratch.size() + 1 < static_cast<std::size_t>(made))
        models.band_scratch.resize(static_cast<std::size_t>(made) - 1);
    for (int32_t band = 1; band < made; ++band) {
        auto& scratch = models.band_scratch[static_cast<std::size_t>(band) - 1];
        model_render::copy_renderer_settings(models.renderer, scratch.renderer);
        model_render::bridge_band_colours(
            models.bridge, models.bands[static_cast<std::size_t>(band)]
        );
    }
    // Each band notes its own failure; the first band's that failed is
    // thrown, whichever thread failed first.
    std::array<std::string, oa::platform::job_pool::max_threads> failures{};
    oa::platform::job_pool::run_bands(
        made > 1 ? pool : nullptr, static_cast<uint32_t>(made), [&](uint32_t band) noexcept {
            std::string& failure = failures[band];
            try {
                if (band == 0) {
                    draw_world_band(
                        models.draws,
                        frame,
                        models.bands.front(),
                        models.renderer,
                        models.supersample,
                        models.debris_points
                    );
                    return;
                }
                auto& scratch = models.band_scratch[band - 1];
                draw_world_band(
                    models.draws,
                    frame,
                    models.bands[band],
                    scratch.renderer,
                    scratch.supersample,
                    scratch.debris_points
                );
            } catch (const std::exception& error) {
                failure = error.what()[0] != '\0' ? error.what() : "a band failed";
            } catch (...) {
                failure = "a band failed";
            }
        }
    );
    for (auto& band : models.bands)
        model_render::bridge_join_band(models.bridge, band);
    for (int32_t band = 0; band < made; ++band)
        if (!failures[static_cast<std::size_t>(band)].empty())
            throw std::runtime_error(
                "cannot draw the battlefield: " + failures[static_cast<std::size_t>(band)]
            );
}

/// The build effect the building being placed is drawn with at a tick of
/// its pulse (ui.build-preview, view_rules::build_preview_look): its
/// outline, its fill or nothing, and the scanline over the heights the
/// look lights, counted from the lowest point of its visible pieces.
///
/// @param model the building, its pieces placed for the frame
/// @param pulse_tick ticks since its pulse started
/// @param fill the preview fills the model
/// @return the bands its image is drawn with
model_render::BuildEffectBands
build_preview_bands(const model_render::ModelRef& model, uint32_t pulse_tick, bool fill) {
    constexpr uint16_t visible = static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible);
    constexpr int32_t whole_shift = 16;
    std::optional<std::pair<int32_t, int32_t>> heights;
    for (const auto& piece : model.instance->pieces()) {
        if ((piece.flags & visible) == 0)
            continue;
        for (const auto& vertex : piece.transformed_vertices) {
            const int32_t height = vertex.y >> whole_shift;
            if (!heights)
                heights = std::pair{height, height};
            heights->first = std::min(heights->first, height);
            heights->second = std::max(heights->second, height);
        }
    }
    const auto [lowest, highest] = heights.value_or(std::pair{0, 0});
    const auto look = view_rules::build_preview_look(pulse_tick, fill, highest - lowest);
    // Depths are heights over the model's base, as its image's depth plane
    // holds them.
    const bool digger = model.def != nullptr && (model.def->flags & OA_UNIT_DEF_FLAG_DIGGER) != 0;
    const int32_t base =
        model_render::model_depth_base + (digger ? model_render::digger_depth_bias : 0);
    const auto depth = [&](int32_t up) {
        constexpr int32_t deepest = 0xff;
        return static_cast<uint8_t>(std::clamp(base + lowest + up, 0, deepest));
    };
    model_render::BuildEffectBands bands;
    const int32_t body = look.fill ? int32_t{*look.fill} : model_render::remap_clear;
    bands.below = body;
    bands.band = body;
    bands.above = body;
    bands.outline = look.outline;
    if (look.scanning) {
        bands.band_low = depth(look.scan_low);
        bands.band_end = depth(look.scan_high + 1);
        bands.band = view_rules::build_preview_scan_colour;
    }
    return bands;
}

/// Writes each piece's turn per tick into `turns`, three words a piece in
/// xy, xz, yz order. A unit with no script leaves them zero. Script axis 0
/// is the yz word, axis 1 the xz word and axis 2 the xy word.
void piece_turn_words(oa::sim::match_runtime::UnitInstance& instance, std::vector<int32_t>& turns) {
    const auto count = instance.model().pieces().size();
    turns.assign(count * 3, 0);
    auto* script = instance.script();
    if (script == nullptr)
        return;
    for (std::size_t piece = 0; piece < count; ++piece) {
        const auto at = piece * 3;
        const auto index = static_cast<uint32_t>(piece);
        turns[at] = script->piece_turn_speed(index, 2);
        turns[at + 1] = script->piece_turn_speed(index, 1);
        turns[at + 2] = script->piece_turn_speed(index, 0);
    }
}

} // namespace

unit_playout::FrameTime
playout_moment(const MatchPresentation& presentation, uint32_t fraction) noexcept {
    return unit_playout::frame_time_between(
        presentation.tick - presentation.batch, presentation.tick, fraction
    );
}

std::optional<unit_playout::UnitPose> mirrored_pose(
    const MatchModels& models, const oa::World& world, uint16_t slot, unit_playout::FrameTime moment
) noexcept {
    const auto* playout = models.presentation.playout;
    if (playout == nullptr || !models.presentation.seen)
        return std::nullopt;
    const oa::Unit* unit = oa::world_unit_at(&world, slot);
    if (unit == nullptr || unit->type_index == 0 || !mirrored_owner(world, *unit))
        return std::nullopt;
    const oa::Unit* carrier =
        unit->attach_parent != 0 ? oa::world_unit(&world, unit->attach_parent) : nullptr;
    if (carrier == nullptr)
        return playout->unit_pose(slot, moment);
    // A carried unit goes where its carrier is drawn.
    const auto carrier_pose =
        mirrored_owner(world, *carrier)
            ? playout->unit_pose(
                  static_cast<uint32_t>(oa::world_unit_slot(&world, carrier)), moment
              )
            : std::nullopt;
    if (!carrier_pose)
        return std::nullopt;
    // Moved by as much as its carrier is drawn away from the carrier's place.
    const auto moved_with = [](int32_t at, int32_t carrier_at, int32_t carrier_drawn) {
        return wrapping_sub(at, wrapping_sub(carrier_at, carrier_drawn));
    };
    unit_playout::UnitPose moved{};
    moved.position = {
        moved_with(unit->position.x, carrier->position.x, carrier_pose->position.x),
        moved_with(unit->position.y, carrier->position.y, carrier_pose->position.y),
        moved_with(unit->position.z, carrier->position.z, carrier_pose->position.z)
    };
    moved.heading = unit->heading;
    moved.pitch = unit->pitch;
    moved.bank = unit->bank;
    return moved;
}

FixedVec3 shown_unit_position(
    const MatchModels& models, const oa::World& world, uint16_t slot, uint32_t fraction
) {
    if (const auto pose =
            mirrored_pose(models, world, slot, playout_moment(models.presentation, fraction)))
        return pose->position;
    if (fraction < whole_tick && unit_moved(models, slot)) {
        const auto& motion = models.presentation.units[slot];
        return blend_point(motion.previous.position, motion.current.position, fraction);
    }
    const oa::Unit* unit = oa::world_unit_at(&world, slot);
    return unit != nullptr ? unit->position : FixedVec3{};
}

FixedVec3 shown_unit_position(const MatchModels& models, const oa::World& world, uint16_t slot) {
    return shown_unit_position(models, world, slot, models.presentation.fraction);
}

void observe_match_tick(MatchModels& models, oa::sim::match_runtime::Match& match) {
    const uint32_t tick = match.simulation().tick;
    auto& presentation = models.presentation;
    if (!presentation.seen || presentation.tick != tick) {
        const uint32_t ticks = presentation.seen ? batch_ticks(presentation.tick, tick) : 0;
        presentation.batch = std::max<uint32_t>(ticks, 1);
        presentation.tick = tick;
        presentation.seen = true;
    }
    auto& world = match.world();
    const uint32_t slots = world.record.unit_slot_count;
    if (presentation.units.size() < slots)
        presentation.units.resize(slots);
    std::vector<int32_t> turns;
    for (uint32_t slot = 1; slot < slots; ++slot) {
        auto& motion = presentation.units[slot];
        const oa::Unit& record = world.record.units[slot];
        auto* runtime =
            record.type_index != 0 ? &match.runtime_state(static_cast<uint16_t>(slot)) : nullptr;
        if (runtime == nullptr || !runtime->instance) {
            if (motion.seen)
                forget_unit(motion);
            continue;
        }
        // The redraw as the save dialog closed once crashed reading the pieces of
        // an instance (a mod's game on Linux), for a cause not found: an instance
        // that is not the unit's own is left out of the motion, and logged once.
        const auto& instance = runtime->instance->model();
        const auto* owner = match.spawn_slot(slot).unit;
        if (!instance_fits_unit(instance, owner)) {
            if (motion.seen)
                forget_unit(motion);
            if (!presentation.misfit_logged) {
                presentation.misfit_logged = true;
                std::cerr << "open-annihilation: tick " << tick << ": unit slot " << slot
                          << "'s model instance is not its own (owner "
                          << (instance.owner_token() == reinterpret_cast<uintptr_t>(owner)
                                  ? "its unit"
                                  : "another")
                          << ", " << instance.pieces().size()
                          << " pieces); left out of the motion (logged once a match)\n";
            }
            continue;
        }
        piece_turn_words(*runtime->instance, turns);
        observe_unit(
            motion, tick, runtime->instance_generation, record, runtime->instance->model(), turns
        );
    }
    observe_shots(presentation.shots, tick, match.projectiles());
    const auto& effects = match.effects();
    observe_debris(presentation.debris, tick, effects.debris);
}

void Runtime::place_tracking_camera() {
    if (screen_ != Screen::match || match_paused_ || !match_ || !match_tracking_ || director_mode())
        return;
    const auto& world = match_->world().record;
    if (oa::world_unit_at(&world, tracked_match_unit_) == nullptr)
        return;
    auto& models = match_models();
    observe_match_tick(models, *match_);
    const FixedVec3 shown = shown_unit_position(
        models, world, tracked_match_unit_, tick_fraction(presentation_alpha())
    );
    centre_view_on(static_cast<int32_t>(shown.x), static_cast<int32_t>(shown.z));
}

const oa::formats::gaf::RenderedFrame*
Runtime::feature_sequence_image(oa_ref32 sequence, uint16_t frame) {
    if (sequence == 0 || sequence > feature_assets_.sequences.size())
        return nullptr;
    auto& rendered = feature_assets_.rendered[sequence - 1];
    if (rendered.empty())
        if (const auto decoded = decode_feature_sequence(sequence))
            for (const auto& source : decoded->frames) {
                auto image = oa::formats::gaf::render_normal(source);
                rendered.push_back(
                    image.ok() ? std::move(*image.frame) : oa::formats::gaf::RenderedFrame{}
                );
            }
    return frame < rendered.size() && !rendered[frame].pixels.empty() ? &rendered[frame] : nullptr;
}

MatchModels& Runtime::match_models() {
    if (match_models_ && match_models_->match == match_.get())
        return *match_models_;
    match_models_ = std::make_shared<MatchModels>();
    auto& fresh = *match_models_;
    fresh.match = match_.get();
    fresh.offline = match_.get();
    // Feature draw states belong to the renderer that began them, as the
    // unit states in `fresh` do.
    for (auto& feature : match_features_)
        feature.state = {};
    fresh.library.textures = model_render::load_texture_library(assets_);
    model_render::build_model_display(
        fresh.display, oa::present::palette_from_bytes(match_palette_)
    );
    oa::present::copy_alpha_table(fresh.display.context, display_.context.alpha_table);
    fresh.renderer.composite_limits = limits_.model_composite;
    model_render::init_composite_buffer(fresh.renderer);
    fresh.renderer.user = &fresh;
    fresh.renderer.ground_height = terrain_height;
    fresh.renderer.model_of = carried_model;
    // The units of other machines' players are drawn on their playout.
    fresh.presentation.playout = &unit_playout_;
    fresh.feature_unit.flags = OA_UNIT_FLAG_VIEWPOINT_OWNED | OA_UNIT_FLAG_BUILDING;
    fresh.feature_unit.flags2 = OA_UNIT_FLAG2_Z_BUFFER;
    // The first record of the type table Game.unit_defs refers to. The
    // World keeps that table in unit_defs, so until the match binds
    // Game.unit_defs the table start is record 0.
    const auto& game = match_->world().record.game;
    fresh.feature_unit.def = game.unit_defs != 0 ? game.unit_defs : oa::oa_ref_from_index(0);
    if (model_light_)
        std::copy(model_light_->begin(), model_light_->end(), std::begin(fresh.renderer.light));
    if (const auto* shadow = gaf_sequence(match_fx_, "shadow");
        shadow != nullptr && !shadow->frames.empty() && shadow->frames.front().layers.empty() &&
        shadow->frames.front().pixels.size() ==
            static_cast<std::size_t>(shadow->frames.front().width) *
                shadow->frames.front().height) {
        const auto& frame = shadow->frames.front();
        fresh.shadow_pixels = frame.pixels;
        fresh.projectile_shadow.width = frame.width;
        fresh.projectile_shadow.height = frame.height;
        fresh.projectile_shadow.origin_x = frame.origin_x;
        fresh.projectile_shadow.origin_y = frame.origin_y;
        fresh.projectile_shadow.key = frame.transparency_index;
        fresh.projectile_shadow.data = fresh.shadow_pixels.data();
    }
    return fresh;
}

void Runtime::release_model_images() {
    if (!match_models_)
        return;
    // Images drawn finer (enhanced anti-aliasing) go with the game's.
    for (auto& tracked : match_models_->units) {
        tracked.state.image = {};
        tracked.state.shadow = {};
        tracked.state.finer.reset();
    }
    for (auto& motion : match_models_->presentation.units) {
        motion.state.image = {};
        motion.state.shadow = {};
        motion.state.finer.reset();
    }
    for (auto& feature : match_features_) {
        feature.state.image = {};
        feature.state.shadow = {};
    }
}

std::size_t Runtime::cached_model_images() const {
    if (!match_models_)
        return 0;
    std::size_t count = 0;
    for (const auto& tracked : match_models_->units)
        count += tracked.state.image.sprite.data != nullptr ? 1 : 0;
    for (const auto& motion : match_models_->presentation.units)
        count += motion.state.image.sprite.data != nullptr ? 1 : 0;
    for (const auto& feature : match_features_)
        count += feature.state.image.sprite.data != nullptr ? 1 : 0;
    return count;
}

uint8_t Runtime::match_palette_index(uint8_t r, uint8_t g, uint8_t b) {
    return model_render::bridge_index(match_models().bridge, r, g, b);
}

const oa::present::DisplayContext& Runtime::match_display_context() {
    return match_models().display.context;
}

void Runtime::set_match_model_light(int32_t x, int32_t y, int32_t z) {
    auto& renderer = match_models().renderer;
    model_render::set_model_light(renderer, x, y, z);
    model_light_ = {renderer.light[0], renderer.light[1], renderer.light[2]};
}

int32_t Runtime::loaded_match_primitives(
    const oa::formats::objects3d::Model& model,
    uint32_t object,
    std::vector<oa::sim::effect_particles::PiecePrimitive>& out
) {
    out.clear();
    const auto* loaded =
        prepared_type_model(match_models().library, loaded_commander_types_, model);
    if (loaded == nullptr)
        return -1;
    const auto& prepared = loaded->objects.at(object);
    for (const auto& primitive : prepared.primitives) {
        const auto& source = model.objects[object].primitives[primitive.source_index];
        out.push_back(
            {source.color_index,
             static_cast<uint32_t>(source.vertex_indices.size()),
             source.vertex_indices.data(),
             primitive.flags}
        );
    }
    return prepared.skips_first ? 0 : -1;
}

std::optional<std::size_t>
Runtime::greyed_picture_frame(const oa::ui::gui_layout::Gadget& gadget) const {
    constexpr uint32_t last_frame_attribute = 0x100U;
    constexpr uint32_t first_frame_attributes = 0x1800U;
    const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
    if (button == nullptr || !button->grayed_out || button->stages != 0)
        return std::nullopt;
    const auto* sequence = gaf_sequence(match_hud_->sprites, gadget.common.name);
    if (sequence == nullptr)
        sequence = gaf_sequence(match_hud_->shared_sprites, gadget.common.name);
    if (sequence == nullptr || sequence->frames.empty())
        return std::nullopt;
    const std::size_t last = sequence->frames.size() - 1;
    const auto attributes = static_cast<uint32_t>(gadget.common.attributes);
    if ((attributes & last_frame_attribute) != 0)
        return last;
    if ((attributes & first_frame_attributes) != 0)
        return 0;
    return std::min(static_cast<std::size_t>(std::max<int16_t>(button->status, 0)) + 2U, last);
}

renderer::ButtonCondition Runtime::match_button_condition(std::size_t index) const {
    const auto& gadget = match_hud_->layout.gadgets[index];
    if (greyed_picture_frame(gadget))
        return renderer::ButtonCondition::disabled;
    // The order page's status buttons draw a grayed state instead of hiding.
    const bool status = match_status_frame(index).has_value();
    const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
    // A grayed-out button, such as SAVEGAME when the game cannot save or an
    // order the selection cannot give, shows grayed rather than hidden.
    if (!status && button != nullptr && button->grayed_out)
        return renderer::ButtonCondition::disabled;
    // The pointer over a button leaves it as it is: a button shows pressed
    // while a press on it is held with the pointer still over it, or while
    // its order is lit.
    if ((match_hud_held_ == index && hovered_ == index) || match_command_lit(index))
        return renderer::ButtonCondition::pressed;
    return renderer::ButtonCondition::normal;
}

Runtime::BackedText::BackedText(Runtime& owner)
    : runtime(&owner), exceptions(std::uncaught_exceptions()) {
    owner.text_hold_.open(owner.text_backings_shown());
}

Runtime::BackedText::~BackedText() {
    // A frame an exception leaves lays nothing more.
    if (std::uncaught_exceptions() > exceptions)
        runtime->text_hold_.drop();
    else
        runtime->text_hold_.close();
}

bool Runtime::text_backings_shown() const {
    const auto settings = game_text_settings();
    // A line's box comes with the lines the modern fonts draw, whole or the
    // characters a game font lacks; the message log's backdrop with either
    // font.
    return (settings.style.background && modern_fonts_open()) || chat_backdrop_shown();
}

void Runtime::hold_painted_text(std::function<void()> paint) {
    text_hold_.hold([this,
                     target = overlay_target_,
                     source_space = hud_source_space_,
                     origin = paint_origin_,
                     painter = std::move(paint)] {
        // Painted where the line was drawn, and the frame's paint target
        // given back after it.
        struct KeptPaint {
            Runtime* runtime{};
            renderer::Surface* kept_target{};
            bool kept_source_space{};
            oa::ui::display_layout::Point kept_origin{};

            ~KeptPaint() {
                runtime->overlay_target_ = kept_target;
                runtime->hud_source_space_ = kept_source_space;
                runtime->paint_origin_ = kept_origin;
            }
        } const kept{this, overlay_target_, hud_source_space_, paint_origin_};
        overlay_target_ = target;
        hud_source_space_ = source_space;
        paint_origin_ = origin;
        painter();
    });
}

void Runtime::compose_match_hud() {
    quick_keys_applied_.clear();
    // The HUD is drawn into the memory of the layer the frame before last
    // showed, so that a frame takes none of its own.
    renderer::Surface hud = std::move(spare_match_hud_);
    if (director_ != nullptr && !director_->presentation.show_interface) {
        // No interface: the HUD layer stays empty.
        hud.width = 0;
        hud.height = 0;
        hud.rgb.clear();
    } else if (match_hud_) {
        // The bars reach the window's right edge: past 640 columns on a
        // window wider than the interface, or beside a narrowed side column.
        extend_match_bars(kBattlefieldLeft + match_layout_.bar_columns());
        std::vector<renderer::ButtonPresentation> buttons;
        for (std::size_t index = 0; index < match_hud_->layout.gadgets.size(); ++index) {
            if (const auto frame = greyed_picture_frame(match_hud_->layout.gadgets[index])) {
                buttons.push_back(
                    {match_hud_->layout.gadgets[index].common.name,
                     renderer::ButtonCondition::disabled,
                     frame,
                     std::nullopt,
                     std::nullopt}
                );
                continue;
            }
            // The order page's status buttons draw a grayed state instead of hiding.
            const auto status_frame = match_status_frame(index);
            const auto condition = match_button_condition(index);
            // A multi-stage button (RESTART.GUI's Difficulty) shows its stage.
            const auto& gadget = match_hud_->layout.gadgets[index];
            std::optional<std::size_t> stage;
            if (const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
                button != nullptr && button->stages > 1) {
                if (const auto found = widget_text_stages_.find(gadget.common.name);
                    found != widget_text_stages_.end())
                    stage = found->second;
            }
            buttons.push_back(
                {gadget.common.name,
                 condition,
                 status_frame ? status_frame : stage,
                 stage,
                 std::nullopt}
            );
            // The button's quick key is underlined in its caption, and the
            // record holding the keyboard focus is ringed while the match's
            // panels take the keyboard.
            if (const auto* button =
                    std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields)) {
                buttons.back().quick_key = static_cast<char>(button->quick_key);
                // An order button's underline follows the chord that fires it.
                // A chord with a modifier, or a letter the caption does not
                // hold, underlines nothing, so a moved key leaves no stale letter.
                const std::string_view action = order_gadget_action(index);
                if (!action.empty()) {
                    if (const std::optional<char> letter =
                            order_quick_letter(action, button->text)) {
                        buttons.back().quick_key = *letter;
                        quick_keys_applied_.push_back(
                            QuickKeyApplied{gadget.common.name, std::string(action), *letter}
                        );
                    }
                }
            }
            buttons.back().focused =
                match_panels_keyboard_ && static_cast<int32_t>(index) == match_hud_focus_;
        }
        // A team panel's logos, row icons and recipient list.
        std::vector<renderer::ListPresentation> lists;
        present_team_panel(buttons, lists);
        const auto hud_start = std::chrono::steady_clock::now();
        renderer::render_screen_into(hud, *match_hud_, buttons, lists);
        if (auto* scrolls = hud_scrolls()) {
            renderer::refresh_layout_scrolls(*scrolls, match_hud_->layout);
            renderer::draw_layout_scrolls(
                hud,
                renderer::grayed_paint(*match_hud_, hud_gray_table_),
                &hud_own_art_,
                match_hud_->sprites,
                *scrolls,
                0,
                0
            );
        }
        // A placed dialog's panels below go under it as it is composed.
        if (match_hud_placement_ == 0)
            compose_panels_below(hud);
        place_preferences_rows(hud);
        compose_match_dialog(hud);
        phase_times_.hud += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - hud_start
        )
                                .count();
    } else if (
        match_chrome_.width == static_cast<uint32_t>(kCanvasWidth) &&
        match_chrome_.height == static_cast<uint32_t>(kCanvasHeight) &&
        match_chrome_.rgb.size() == static_cast<std::size_t>(kCanvasWidth * kCanvasHeight * 3)
    ) {
        hud.width = match_chrome_.width;
        hud.height = match_chrome_.height;
        hud.rgb.assign(match_chrome_.rgb.begin(), match_chrome_.rgb.end());
    } else {
        hud.width = static_cast<uint32_t>(kCanvasWidth);
        hud.height = static_cast<uint32_t>(kCanvasHeight);
        hud.rgb.assign(static_cast<std::size_t>(kCanvasWidth * kCanvasHeight * 3), 0);
    }
    spare_match_hud_ = std::move(match_hud_cpu_);
    match_hud_cpu_ = std::move(hud);
    ++accelerated_.hud_revision;
}

void Runtime::render_match_surface() {
    // A match frame draws every line of text it shows, so the layers kept of
    // the lines the frame before did not draw, such as a readout's earlier
    // numbers, go.
    oa::present::forget_undrawn_text_layers();
    // A failed rotated frame puts the view back classic, so the second draw
    // draws the battlefield as the classic tiers do.
    if (!draw_match_surface())
        std::ignore = draw_match_surface();
}

bool Runtime::draw_match_surface() {
    if (!match_ || !selected_tnt_)
        throw std::logic_error("match renderer requires an initialized offline match");
    // The drawing rebuilds piece placements the match reads, so it rounds as
    // the ticks do.
    oa::base::float_precision::restore_program_float_control();
    mark_profile(OA_PROFILE_MISC);
    sync_match_wrecks();
    follow_shared_cameras();
    keep_room_for_clock_line();
    // ui.interface-fixes cursor-reset: an armed command, build placement
    // included, is dropped once the unit the order panel shows is gone.
    if (const auto& fixes = ui_rules().interface_fixes;
        fixes.enabled &&
        fixes.fixes.contains(oa::data::mod_profile::UiInterfaceFixesFixes::cursor_reset) &&
        match_command_ != MatchCommand::none && oa::ui::hud::panel_unit_vanished(match_->state())) {
        reset_match_command();
        pending_build_type_ = 0;
    }
    // Director mode draws from the director's camera, which it keeps on the
    // map itself, and without the interface unless its presentation asks
    // for it; its draws never start debris particles.
    const bool directed = director_ != nullptr;
    const bool bare = directed && !director_->presentation.show_interface;
    // Zoomed out past the zoom the tier draws units whole at, the frame is
    // the far view: the processor draws it at the zoom in every tier, the
    // terrain from the map's pyramid.
    const bool far = !directed && !live_renderer_active() && far_view_frame();
    // Zoomed out past After zoom with Zoomed out units at Dots, or at any
    // zoom where the models would take too much memory, each unit is a dot
    // of its owner's colour and no model is drawn at all. A director's frame
    // stays rendered until its picture cannot be allocated.
    bool dots = !directed && !live_renderer_active() && dots_frame();
    // The map the view shows: the fill below draws nothing beyond it. The
    // player's camera is held within the view's limits, which let it go
    // past the map's edges (view_camera), and the limits then hold the next
    // view from it; the director's is held on the map's start.
    const auto [map_width, map_height] = shown_map_size();
    const auto bf_w = match_layout_.battlefield_width();
    const auto bf_h = match_layout_.battlefield_height();
    const auto vis_w = visible_map_width();
    const auto vis_h = visible_map_height();
    // The terrain, the draws and the fog go into the scene at the draw scale;
    // unless it is drawn apart, the scene is the world layer at the zoom.
    const auto scaling = world_scaling();
    const float draw_scale = scaling.draw_scale;
    // In the Full tier the graphics card draws the whole battlefield: the
    // terrain is not filled, the bands do not draw and the fog is not
    // rasterised; the world layer is the overlay canvas, cleared to the key
    // colour, which the painters after the fog paint. The camera and the
    // zoom the frame draws at are kept for the card's frame.
    // A director's frame is the card's only on the renderer with no window
    // that draws director frames on the graphics card.
    const bool card_world = full_presentation() && (!directed || director_draws_on_card()) &&
                            !far && !live_renderer_active();
    // The live renderer consumes the draw list directly. Its GPU picture replaces
    // the whole battlefield, so a classic terrain, model and fog raster is unused.
    // A director's turned frame on the graphics card is the live renderer's.
    const bool live_world = (!directed || director_draws_on_card()) && live_renderer_active() &&
                            !live_renderer_failed();
    // How far between map pixels the frame draws the view: an accelerated
    // frame the card draws, magnifies or the area pass reduces may, and so
    // may a scene the processor draws at a zoom below 1, which lays its
    // ground from the view's phase; every other frame draws it on the
    // camera's map pixel, as the game always has. Settled before the camera
    // is held, so that a frame that changes it takes its camera by its own
    // rule.
    const bool processor_below_one = !card_world && scaling.method == SceneMethod::none &&
                                     draw_scale > 0.0F && draw_scale < 1.0F;
    const bool between_pixels =
        !directed &&
        ((accelerated_presentation() && (card_world || scaling.method == SceneMethod::magnify ||
                                         scaling.method == SceneMethod::area)) ||
         processor_below_one);
    // The card's frames below zoom 1 draw the picture on the grid and move
    // it on to the view's exact place by the rest of a screen pixel.
    std::ignore = settle_view_offset(
        between_pixels,
        card_world || processor_below_one,
        card_world && match_zoom() > 0.0F && match_zoom() < 1.0F
    );
    const auto held = view_camera();
    const int32_t camera_x = directed ? std::max(0, match_camera_x_) : held[0];
    const int32_t camera_y = directed ? std::max(0, match_camera_z_) : held[1];
    match_camera_x_ = camera_x;
    match_camera_z_ = camera_y;
    if (!directed) {
        // The limits hold the next view from this one's exact place, which
        // its camera is taken from, so that its rounding never moves them.
        const auto zoom = static_cast<double>(match_zoom() <= 0.0F ? 1.0F : match_zoom());
        const auto place = match_view_place();
        view_hold_ = {
            true,
            place[0] + static_cast<double>(bf_w) / zoom / 2.0,
            place[1] + static_cast<double>(bf_h) / zoom / 2.0
        };
    }
    // The frame's place between the ticks. Each draw notes the tick the
    // match is on first, so that the poses of the tick before are at hand.
    auto& models = match_models();
    observe_match_tick(models, *match_);
    auto& presentation = models.presentation;
    presentation.fraction = tick_fraction(presentation_alpha());
    // The moment the units of other machines' players are drawn at on their
    // playout, which the pointer then picks against.
    presentation.drawn_moment = playout_moment(presentation, presentation.fraction);
    const auto moment = presentation.drawn_moment;

    // Outside a draw, every unit shows where its tick puts it, a mirrored
    // unit where its playout has it at the tick.
    struct WholeTickAfter {
        MatchPresentation& presentation;

        explicit WholeTickAfter(MatchPresentation& shown) : presentation(shown) {}

        WholeTickAfter(const WholeTickAfter&) = delete;
        WholeTickAfter& operator=(const WholeTickAfter&) = delete;

        ~WholeTickAfter() { presentation.fraction = whole_tick; }
    } whole_tick_after{presentation};

    const bool between_ticks = presentation.fraction < whole_tick;
    // Each draw places the copies it draws from afresh: a mirrored unit draws
    // from them on whole ticks too.
    ++presentation.draw;
    {
        const uint32_t tick = match_->simulation().tick;
        const auto shots = match_->projectiles();
        presentation.presented_shots.resize(shots.size());
        for (std::size_t index = 0; index < shots.size(); ++index)
            presentation.presented_shots[index] = presented_shot(
                presentation.shots, tick, index, shots[index], presentation.fraction
            );
    }
    ensure_radar_surfaces();
    if (directed)
        bind_director_view();
    else
        bind_match_view();
    // The frame's on-screen list, after the ticks and before the drawing: the
    // pointer and the under-attack notice test the list the frame drawn last
    // built. In director mode the notice tests the director's view instead,
    // so that it does not hang on which frames were drawn.
    rebuild_on_screen_units();
    if (directed)
        offline_services_.set_on_screen_test(&DirectorState::unit_in_view, &match_->state());
    else
        track_match_drag();
    auto viewport = live_viewport(camera_x, camera_y);
    match_use_layers_ =
        (sdl_.renderer != nullptr && !options_.headless_check) || director_draws_on_card();
    // The sweep that has just opened the menu shows the sound page on this
    // frame, so the HUD drawn below is that page and not the tabs alone.
    advance_options_lightbar();
    compose_match_hud();
    // What the presentation and the picture-keeping readers learn of the frame.
    accelerated_.frame = scaling;
    accelerated_.frame_alpha = presentation_alpha();
    const auto drawn_offset = view_offset();
    // The processor's scene starts that far into the camera's map pixel;
    // what it draws carries the offset, so the presentation moves it no
    // further.
    const auto phase = scene_phase(scaling);
    accelerated_.frame_offset =
        processor_below_one ? oa::present::world_renderer::ViewOffset{} : drawn_offset;
    accelerated_.frame_shift = card_world ? view_shift() : std::array<double, 2>{};
    const int32_t scene_w = scaling.scene_width;
    const int32_t scene_h = scaling.scene_height;
    const auto terrain_pixels =
        static_cast<std::size_t>(scene_w) * static_cast<std::size_t>(scene_h) * 3U;
    if (match_terrain_cache_.width != static_cast<uint32_t>(scene_w) ||
        match_terrain_cache_.height != static_cast<uint32_t>(scene_h) ||
        match_terrain_cache_.rgb.size() != terrain_pixels) {
        match_terrain_cache_.width = static_cast<uint32_t>(scene_w);
        match_terrain_cache_.height = static_cast<uint32_t>(scene_h);
        match_terrain_cache_.rgb.resize(terrain_pixels);
        terrain_cache_cam_x_ = kUncachedTerrainCamera;
    }
    note_full_canvas(card_world, camera_x, camera_y, draw_scale);
    // A director's camera redraws the terrain at any change of zoom, however
    // small, so that what a frame shows never hangs on the frames before it.
    if (card_world || live_world) {
        // The cache is not filled for this frame: the next frame drawn
        // whole fills it.
        terrain_cache_cam_x_ = kUncachedTerrainCamera;
    } else if (
        terrain_cache_cam_x_ != camera_x || terrain_cache_cam_y_ != camera_y ||
        std::abs(terrain_cache_zoom_ - draw_scale) > 1.0e-4F ||
        (directed && terrain_cache_zoom_ != draw_scale) || terrain_cache_phase_ != phase
    ) {
        if (auto error = oa::present::world_renderer::fill_scaled_viewport(
                *selected_tnt_,
                match_palette_,
                camera_x,
                camera_y,
                static_cast<uint32_t>(map_width),
                static_cast<uint32_t>(map_height),
                static_cast<uint32_t>(scene_w),
                static_cast<uint32_t>(scene_h),
                draw_scale,
                match_terrain_cache_.rgb.data(),
                static_cast<uint32_t>(scene_w),
                draw_pool_.get(),
                phase[0],
                phase[1]
            ))
            throw std::runtime_error("cannot render match terrain: " + error->message);
        terrain_cache_cam_x_ = camera_x;
        terrain_cache_cam_y_ = camera_y;
        terrain_cache_zoom_ = draw_scale;
        terrain_cache_phase_ = phase;
    }
    // The world layer is the battlefield alone; the HUD stays in 640x480
    // source space until presentation (or compose_match_frame) scales it.
    auto& scene_layer = scaling.apart ? match_scene_cpu_ : match_world_cpu_;
    scene_layer.width = static_cast<uint32_t>(scene_w);
    scene_layer.height = static_cast<uint32_t>(scene_h);
    scene_layer.rgb.resize(terrain_pixels);
    if (card_world) {
        fill_rgb24(scene_layer.rgb.data(), terrain_pixels / 3U, full_overlay_key());
        note_full_canvas_pixels(scene_layer.rgb.data());
    } else if (!live_world) {
        std::memcpy(scene_layer.rgb.data(), match_terrain_cache_.rgb.data(), terrain_pixels);
    }
    viewport.destination_x = 0;
    viewport.destination_y = 0;
    viewport.surface_width = static_cast<uint32_t>(bf_w);
    viewport.surface_height = static_cast<uint32_t>(bf_h);
    // The scene's view: the battlefield's camera and corner at the draw
    // scale. Units and features are culled through the battlefield's view,
    // at the zoom, and drawn through the scene's.
    auto scene_view = viewport;
    scene_view.scale = draw_scale;
    scene_view.width = static_cast<uint32_t>(scene_w);
    scene_view.height = static_cast<uint32_t>(scene_h);
    scene_view.surface_width = static_cast<uint32_t>(scene_w);
    scene_view.surface_height = static_cast<uint32_t>(scene_h);
    world_pixel_clip_ = {0, 0, scene_w, scene_h};
    oa::present::world_renderer::Surface world_surface{
        scene_layer.width, scene_layer.height, std::move(scene_layer.rgb)
    };
    // Past the tier's floor a unit is a few pixels at most: the margin
    // keeps to the map pixels a large unit and its shadow reach.
    const auto cull_margin =
        live_renderer_active() ? 2 * std::max(bf_w, bf_h) + kLeastCullMargin
        : far ? static_cast<int>(
                    std::ceil(kFarCullMarginMapPixels * static_cast<double>(match_zoom()))
                )
              : std::max(
                    kLeastCullMargin,
                    static_cast<int>(
                        std::lround(kCullMarginMapPixels * static_cast<double>(match_zoom()))
                    )
                );
    const auto on_battlefield = [&](int x, int y) {
        return x >= viewport.destination_x - cull_margin &&
               x < viewport.destination_x + bf_w + cull_margin &&
               y >= viewport.destination_y - cull_margin &&
               y < viewport.destination_y + bf_h + cull_margin;
    };
    sync_dead_feature_draws();
    // The model of a weapon registry slot's shots, loaded on first use.
    const auto weapon_model = [&](
                                  MatchModels& models, uint16_t index
                              ) -> const std::shared_ptr<const oa::formats::objects3d::Model>& {
        if (const auto found = models.weapon_models.find(index);
            found != models.weapon_models.end())
            return found->second;
        std::shared_ptr<const oa::formats::objects3d::Model> model;
        if (const auto name = weapon_model_names_.find(index); name != weapon_model_names_.end()) {
            try {
                const auto path = "objects3d/" + name->second + ".3do";
                const auto bytes = assets_.read(path).bytes;
                model =
                    std::make_shared<const oa::formats::objects3d::Model>(oa::ui::decoded::require(
                        oa::formats::objects3d::load_3do(std::as_bytes(std::span(bytes))), path
                    ));
            } catch (const std::exception& error) {
                // The slot keeps no model, so its shots draw without one and
                // the load is not tried again.
                std::cerr << "weapon model objects3d/" << name->second << ".3do: " << error.what()
                          << '\n';
            }
        }
        return models.weapon_models.emplace(index, std::move(model)).first->second;
    };
    // The frame's battlefield draws, worked out in order before any is drawn
    // (world_draws.hpp): everything drawing builds or changes on the way,
    // the piece transforms, the units' cached images, the texture
    // animations, the decoded particle frames, is done here, once, and every
    // band then draws the list.
    auto& draw_list = models.draws;
    clear_world_draws(draw_list);
    draw_list.spatial_particles.clear();
    // The features are cut off where the map the view shows ends.
    draw_list.shown_map_width = map_width;
    draw_list.shown_map_height = map_height;
    const auto add_draw = [&draw_list](WorldDrawKind kind, std::size_t index) {
        add_world_draw(draw_list, kind, index);
    };
    // Projectile render types 1, 3 and 6: the projectile's ground shadow,
    // then its model; a missile also draws its first child (the flame or
    // propeller) while it still has flight time.
    const auto plan_projectile_models = [&](MatchModels& models) {
        auto& renderer = models.renderer;
        const auto shots = match_->projectiles();
        const auto& shown_shots = models.presentation.presented_shots;
        for (std::size_t index = 0; index < shots.size(); ++index) {
            const auto& shot = shots[index];
            const auto* weapon = match_->projectile_weapon(shot);
            if (weapon == nullptr || shot.burst_remaining != 0)
                continue;
            const auto type = weapon->rendertype;
            if (type != kRenderMissile && type != kRenderBomb && type != kRenderModel)
                continue;
            const auto shot_position = oa::sim::match_runtime::fixed_words(shot.position);
            try {
                if (!match_->point_shown(static_cast<uint8_t>(match_view_player()), shot_position))
                    continue;
            } catch (const std::exception&) {
                continue;
            }
            const auto& model = weapon_model(models, weapon->registry_index);
            if (model == nullptr || model->objects.empty())
                continue;
            const auto& prepared = model_render::prepare_model(models.library, model);
            // Where the shot shows in this frame (presented_shot).
            const ShotPose shown =
                shown_shots.size() == shots.size() ? shown_shots[index] : shot_pose(shot);
            const auto shown_position = oa::sim::match_runtime::fixed_words(shown.position);
            ProjectileDraw drawn{};
            drawn.position = {
                wrapping_sub(shown.position.x, camera_fixed(renderer.camera_x)),
                shown.position.y,
                wrapping_sub(shown.position.z, camera_fixed(renderer.camera_y))
            };
            const int32_t x = high_word(drawn.position.x) + renderer.origin_x;
            const int32_t z = high_word(drawn.position.z) + renderer.origin_y;
            const int32_t y = z - (high_word(drawn.position.y) >> 1);
            // As Projectile.plot_height: the terrain height under the shot.
            const int32_t shadow_y =
                z - (match_->map_height(shown_position[0], shown_position[2]) >> 1);
            drawn.region = {
                x - kProjectileReach,
                y - kProjectileReach,
                x + kProjectileReach,
                y + kProjectileReach
            };
            include(
                drawn.region,
                x - kProjectileReach,
                shadow_y - kProjectileReach,
                2 * kProjectileReach,
                2 * kProjectileReach
            );
            drawn.shadow = models.projectile_shadow.data != nullptr && shadows_drawn(draw_list);
            drawn.x = x;
            drawn.shadow_y = shadow_y;
            // Type 3 draws unrotated.
            if (type == kRenderMissile)
                drawn.rotation = {
                    0,
                    static_cast<int16_t>(shown.heading + kHalfTurn),
                    static_cast<int16_t>(shown.pitch + kHalfTurn)
                };
            else if (type == kRenderModel)
                drawn.rotation = {
                    0, static_cast<int16_t>(shown.heading), static_cast<int16_t>(shown.pitch)
                };
            drawn.object = &model->objects[0];
            drawn.prepared = &prepared.objects[0];
            const auto child = model->objects[0].first_child;
            if (type == kRenderMissile && child != oa::formats::objects3d::kNoObject &&
                child < model->objects.size() &&
                static_cast<int32_t>(shot.lifetime_tick) > static_cast<int32_t>(renderer.tick)) {
                drawn.child = &model->objects[child];
                drawn.child_prepared = &prepared.objects[child];
                drawn.child_rotation = drawn.rotation;
                // This match keeps no spin angle for a propeller: it draws
                // with no roll.
                if ((weapon->flags & OA_WEAPON_FLAG_PROPELLER) != 0)
                    drawn.child_rotation.xy = 0;
            }
            draw_list.projectiles.push_back(drawn);
            add_draw(WorldDrawKind::projectile, draw_list.projectiles.size() - 1);
        }
    };

    auto& world_record = match_->world().record;
    auto& renderer = models.renderer;
    renderer.world = &world_record;
    renderer.graphics_flags = world_record.game.graphics_flags;
    // Shadows lighten as the view zooms out and are not drawn from four
    // times as far out as the game's view; at zoom 1 and closer they are
    // the game's own.
    set_frame_shadows(
        draw_list,
        renderer,
        models.shadow_table,
        models.display,
        &models.projectile_shadow,
        match_zoom()
    );
    {
        const auto& fixes = ui_rules().interface_fixes;
        renderer.moving_pieces_once_built =
            fixes.enabled &&
            fixes.fixes.contains(oa::data::mod_profile::UiInterfaceFixesFixes::nanoframe_raster);
    }
    renderer.tick = match_->simulation().tick;
    // The nanoframes pulse by the match's ticks, at 3.1c's rate at zoom 1
    // and in, and slower zoomed out.
    renderer.build_pulse_lag =
        model_render::advance_build_pulse(models.build_pulse, renderer.tick, match_zoom());
    renderer.camera_x = static_cast<int32_t>(viewport.source_x);
    renderer.camera_y = static_cast<int32_t>(viewport.source_y);
    renderer.origin_x = 0;
    renderer.origin_y = 0;
    // Each player's team colour: the owner's PlayerSetupInfo.color.
    for (std::size_t player = 0; player < std::size(renderer.team_colors); ++player) {
        const auto* owner = oa::world_player(&world_record, static_cast<uint32_t>(player));
        const auto* info = owner != nullptr ? oa::world_player_info(&world_record, owner) : nullptr;
        renderer.team_colors[player] = info != nullptr ? info->color : 0;
    }
    if (!models.animation_started || renderer.tick < models.animation_tick) {
        models.animation_started = true;
        models.animation_tick = renderer.tick;
    }
    // Entries of models freed since the last frame go, with their texture
    // animations, before the live ones step.
    model_render::release_expired_models(models.library);
    for (uint32_t step = 0; models.animation_tick < renderer.tick; ++models.animation_tick)
        if (step++ < kMaxAnimationSteps)
            model_render::step_texture_animations(models.library);
    // A unit slot's draw state is found by its place in the slot list,
    // which must not move while the frame's draws point into it: it grows
    // now to hold every slot with a model instance, which is every slot
    // the draws can reach (unit_model).
    hold_unit_draw_states(models);
    // The model bridge holds the scene's map pixels and writes them back at
    // the draw scale.
    const auto scale = scene_view.scale == 0.0F ? 1.0F : scene_view.scale;
    const model_render::RgbFrame rgb_frame{
        world_surface.rgb.data(),
        static_cast<int32_t>(world_surface.width),
        static_cast<int32_t>(world_surface.height),
        static_cast<int32_t>(world_surface.width) * 3
    };
    // A frame of dots draws no model: its bridge holds one pixel, so that
    // its map pixels take no memory however far the view is zoomed out.
    const oa::Rect32 bridge_area{
        scene_view.destination_x,
        scene_view.destination_y,
        dots ? scene_view.destination_x : scene_view.destination_x + scene_w - 1,
        dots ? scene_view.destination_y : scene_view.destination_y + scene_h - 1
    };
    oa::present::DisplayContext* bound_display = oa::present::display_context();
    oa::present::bind_display(&models.display.context);
    if (!live_renderer_active())
        draw_match_debug_grid(models.bridge, models.display, rgb_frame, bridge_area, scale);
    mark_profile(OA_PROFILE_RENDER_STATIC);
    models.bridge.byte_budget = rendered_units_budget();
    if (!model_render::bridge_begin(
            models.bridge, rgb_frame, bridge_area, scale, models.display.palette, phase[0], phase[1]
        )) {
        // One line for the run: the picture could not be held, so this frame
        // draws dots. The process carries on.
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::cerr << "open-annihilation: the units' picture could not be allocated ("
                      << models.bridge.refused_bytes << " bytes); units are drawn as dots\n";
        }
        dots = true;
        const oa::Rect32 one_pixel{
            scene_view.destination_x,
            scene_view.destination_y,
            scene_view.destination_x,
            scene_view.destination_y
        };
        std::ignore = model_render::bridge_begin(
            models.bridge, rgb_frame, one_pixel, scale, models.display.palette, phase[0], phase[1]
        );
    }
    // The "+stats" panel says when this frame drew the units as dots.
    frame_draws_.units_as_dots = dots;
    // Models draw into the bridge and sprites straight into the RGB frame,
    // so the bridge's pixels go back to the frame before a sprite draws over
    // them (a commit), and the next model captures the frame again.
    const auto plan_commit = [&] { add_draw(WorldDrawKind::commit, 0); };
    // Live geometry and shadows do not consume the classic bridge rectangle.
    // Keep it for sampled plans, which use its bounds to prepare finer images.
    const auto model_region = [&](const model_render::ModelRef& model,
                                  model_render::UnitSupersampling level) {
        if (live_world && level == model_render::UnitSupersampling::off)
            return oa::Rect32{};
        return unit_region(renderer, model, terrain_height(&models, model.unit->position));
    };
    // A model readied and planned for the frame's draws.
    const auto add_model = [&](const model_render::ModelRef& model,
                               const oa::Rect32& region,
                               bool idle,
                               model_render::UnitSupersampling level,
                               int32_t stand_in) {
        draw_list.models.emplace_back();
        ModelDraw& drawn = draw_list.models.back();
        drawn.model = model;
        drawn.stand_in = stand_in;
        model_render::plan_unit_supersampled(
            renderer, models.bridge, model, region, idle, level, drawn.plan
        );
        if (live_renderer_active() && drawn.plan.linked.drawn) {
            drawn.shown_position = model.unit->position;
            for (const auto& piece : model.instance->pieces())
                drawn.shown_vertices.push_back(piece.transformed_vertices);
        }
        if (stand_in >= 0)
            drawn.model.unit = nullptr;
        const auto parent_index = draw_list.models.size() - 1;
        add_draw(WorldDrawKind::model, parent_index);
        if (live_world && drawn.plan.linked.drawn) {
            // Classic drawing composites attached units into the parent's
            // image. The live scene needs their own geometry records, using
            // the attachment transforms already prepared with the parent.
            std::vector<model_render::ModelRef> carried;
            for (const auto& child : drawn.plan.model.carried)
                if (child.model.instance && child.model.unit)
                    carried.push_back(child.model);
            const bool unlit = drawn.plan.linked.unlit;
            for (const auto& child : carried) {
                ModelDraw attached;
                attached.model = child;
                attached.plan.linked = {true, unlit};
                attached.shown_position = child.unit->position;
                for (const auto& piece : child.instance->pieces())
                    attached.shown_vertices.push_back(piece.transformed_vertices);
                draw_list.models.push_back(std::move(attached));
            }
        }
    };
    const auto plan_object_feature = [&](MatchFeatureDraw& feature) {
        // A frame of dots draws no feature's model: a wreck or a rock of its
        // own model is a pixel or two there.
        if (dots)
            return;
        // The placed record's object, rotation words and position
        // go to the stand-in unit, which then draws like any unit; the
        // frame's draws keep a copy of it as it draws this feature.
        models.feature_unit.bank = feature.rotation.xy;
        models.feature_unit.heading = static_cast<uint16_t>(feature.rotation.xz);
        models.feature_unit.pitch = feature.rotation.yz;
        models.feature_unit.position = {feature.position.x, feature.position.y, feature.position.z};
        const model_render::ModelRef model{
            &feature.instance,
            &model_render::prepare_model(models.library, feature.instance.model_handle()),
            &feature.state,
            &models.feature_unit,
            oa::world_unit_def(&world_record, models.feature_unit.def)
        };
        model_render::note_piece_changes(model);
        draw_list.stand_ins.push_back(models.feature_unit);
        // Drawn only over the map the view shows.
        add_model(
            model,
            region_on_map(
                model_region(model, model_render::UnitSupersampling::off),
                renderer,
                map_width,
                map_height
            ),
            false,
            model_render::UnitSupersampling::off,
            static_cast<int32_t>(draw_list.stand_ins.size() - 1)
        );
        draw_list.models.back().on_map = true;
    };
    advance_gaf_feature_anims(match_->simulation().tick);
    // plan_feature_draw picks each sprite feature's frames, its
    // shadow first while FeatureShadows is on, and which blend through the
    // alpha table; a record playing a burn, die or reclamate sequence shows
    // the frames it has reached.
    const auto anim_frame = [&](std::size_t index,
                                bool first) -> const oa::formats::gaf::RenderedFrame* {
        if (index >= match_gaf_anims_.size() || match_gaf_anims_[index].frames.empty())
            return nullptr;
        const auto& anim = match_gaf_anims_[index];
        return first ? &anim.frames.front()
                     : &anim.frames[std::min(
                           static_cast<std::size_t>(anim.frame), anim.frames.size() - 1
                       )];
    };
    const auto plan_sprite_feature = [&](const MatchGafFeatureDraw& feature,
                                         const oa::present::model::FeatureDraw& plan,
                                         const oa::present::world_renderer::ScreenPoint& place) {
        plan_commit();
        const auto* plot = oa::world_plot(&world_record, feature.cell_x, feature.cell_z);
        const auto* record =
            plot != nullptr && (plot->flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0
                ? oa::sim::feature_runtime::feature_record(world_record, plot->feature_record)
                : nullptr;
        for (int32_t index = 0; index < plan.sprite_count; ++index) {
            const auto& sprite = plan.sprites[index];
            if (sprite.shadow && !shadows_drawn(draw_list))
                continue;
            const oa::formats::gaf::RenderedFrame* image = nullptr;
            if (sprite.frame == oa::present::model::FeatureFrame::placed) {
                if (record != nullptr) {
                    const auto& cursor =
                        sprite.shadow ? record->sprite.shadow : record->sprite.animation;
                    image = feature_sequence_image(cursor.sequence, cursor.frame);
                }
                // A record whose sequence is not loaded keeps the definition's body.
                if (image == nullptr && !sprite.shadow)
                    image = anim_frame(feature.anim, false);
            } else {
                image = anim_frame(
                    sprite.shadow ? feature.shadow_anim : feature.anim,
                    sprite.frame == oa::present::model::FeatureFrame::first
                );
            }
            if (image == nullptr)
                continue;
            const int8_t height = feature.feature_index < world_record.feature_def_count
                                      ? world_record.feature_defs[feature.feature_index].height
                                      : int8_t{0};
            const bool lies_on_terrain =
                sprite.shadow || !oa::present::world_renderer::feature_stands(height);
            draw_list.sprites.push_back({image, place, sprite.shadow, true, lies_on_terrain});
            draw_list.sprites.back().world_position =
                oa::FixedVec3{feature.position.x, feature.position.y, feature.position.z};
            add_draw(
                sprite.translucent ? WorldDrawKind::blended_sprite : WorldDrawKind::sprite,
                draw_list.sprites.size() - 1
            );
        }
    };
    // Selection boxes go to the bridge: the overlay projects about the
    // game's battlefield corner, the bridge's 8-bit view starts at it, and
    // each line captures the tiles it crosses before drawing.
    oa::present::world_renderer::OverlayRaster selection_raster{};
    selection_raster.user = &draw_list;
    selection_raster.line =
        [](
            void* user, oa::Surface*, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color
        ) {
            auto& list = *static_cast<WorldDrawList*>(user);
            constexpr int32_t left = oa::present::world_renderer::battlefield_origin_x;
            constexpr int32_t top = oa::present::world_renderer::battlefield_origin_y;
            list.lines.push_back({x0 - left, y0 - top, x1 - left, y1 - top, {}, color});
            add_world_draw(list, WorldDrawKind::selection_line, list.lines.size() - 1);
        };

    // While a unit draws from its copies, the units it carries draw from
    // theirs (carried_model).
    struct Blending {
        MatchPresentation& presentation;

        explicit Blending(MatchPresentation& shown) : presentation(shown) {
            presentation.blending = true;
        }

        Blending(const Blending&) = delete;
        Blending& operator=(const Blending&) = delete;

        ~Blending() { presentation.blending = false; }
    };

    // What the frame drew of the units, for "+stats" and a --frame-rate
    // run's frame log: each unit drawn, those drawn from copies placed
    // between two ticks, and where the unit the log follows was drawn.
    const auto note_unit_drawn = [&](uint16_t unit_index, const oa::Unit& drawn, bool blended) {
        ++frame_draws_.units_drawn;
        frame_draws_.units_between_ticks += blended ? 1 : 0;
        if (unit_index == frame_draws_.probe_unit) {
            frame_draws_.probe_drawn = true;
            frame_draws_.probe_x = drawn.position.x;
            frame_draws_.probe_z = drawn.position.z;
        }
    };
    // How finely units are drawn (enhanced anti-aliasing); director frames
    // draw them as without it, and so do frames of the Full tier, whose
    // anti-aliasing is the graphics card's world target, and the far view's,
    // where a unit is a few pixels at most.
    const auto unit_level = directed || full_presentation() || far
                                ? model_render::UnitSupersampling::off
                                : unit_supersampling_;
    // The dots of a frame of dots, drawn over the fog, and the colour of a
    // selected unit's frame: the selection boxes' own.
    std::vector<FarViewDot> far_dots;
    const auto palette_rgb = [this](uint8_t index) {
        const auto at = static_cast<std::size_t>(index) * 4U;
        return std::array<uint8_t, 3>{
            match_palette_[at], match_palette_[at + 1U], match_palette_[at + 2U]
        };
    };
    const auto selected_frame = palette_rgb(
        oa::present::world_renderer::game_ui_color(
            world_record.game, oa::present::world_renderer::ui_color_rotated_box
        )
    );
    const auto plan_unit = [&](uint16_t unit_index) {
        const auto model = unit_model(models, unit_index);
        if (model.instance == nullptr)
            return;
        if (dots) {
            // Where the unit shows: between the ticks, or on another
            // machine's playout, where its copies are drawn.
            const bool shown_elsewhere =
                between_ticks ||
                mirrored_pose(models, world_record, unit_index, moment).has_value();
            const auto screen =
                shown_elsewhere
                    ? project_match_point(
                          viewport,
                          oa::sim::match_runtime::fixed_words(
                              shown_unit_position(models, world_record, unit_index)
                          )
                      )
                    : project_match_point(
                          viewport, oa::sim::match_runtime::fixed_words(model.unit->position)
                      );
            note_unit_drawn(unit_index, *model.unit, shown_elsewhere);
            // A carried unit shows in its carrier's dot.
            if (model.unit->attach_parent != 0)
                return;
            const bool selected = !bare && (model.unit->flags & OA_UNIT_FLAG_SELECTED) != 0;
            far_dots.push_back(
                {screen.x,
                 screen.y,
                 palette_rgb(oa::ui::hud::player_dot_color(world_record, model.unit->owner_index)),
                 selected,
                 selected_frame}
            );
            return;
        }
        // The movement state's speed: an idle mobile shows the first frame
        // of its animated textures.
        bool idle = false;
        if (unit_index < match_->world().slots.size())
            if (const auto* movement = match_->ground_runtime(unit_index))
                idle = movement->movement.speed == 0;
        // A unit of another machine's player draws from its copies on every
        // frame, whole ticks included: they stand on its playout, near its
        // simulated place and moved on ahead of it between records.
        const bool on_playout = mirrored_pose(models, world_record, unit_index, moment).has_value();
        if ((between_ticks && unit_or_cargo_moved(models, world_record, unit_index)) ||
            on_playout) {
            // The match reads the piece transforms a draw rebuilds, so they
            // are rebuilt as a draw of the whole tick rebuilds them, and the
            // unit draws from its copies placed between the ticks. The image
            // cache of the unit's own draw state waits for a draw of a whole
            // tick, which leaves it as it would have been.
            // A carried unit is drawn, and its transforms rebuilt, by its
            // carrier (draw_linked_model).
            const bool carried = model.unit->attach_parent != 0;
            model_render::note_piece_changes(model);
            if (!carried)
                model_render::update_linked_transforms(renderer, model);
            const Blending blending{presentation};
            const auto shown = presented_unit(models, unit_index);
            note_unit_drawn(unit_index, *shown.unit, true);
            if (!bare && (shown.unit->flags & OA_UNIT_FLAG_SELECTED) != 0 &&
                !shown.instance->model().objects.empty())
                oa::present::world_renderer::overlay_selection_box(
                    world_record.game,
                    selection_raster,
                    &models.bridge.surface,
                    *shown.unit,
                    shown.instance->model().objects.front()
                );
            model_render::note_piece_changes(shown);
            if (carried)
                return;
            model_render::update_linked_transforms(renderer, shown);
            add_model(shown, model_region(shown, unit_level), idle, unit_level, -1);
            return;
        }
        note_unit_drawn(unit_index, *model.unit, false);
        // A selected unit's box is outlined before the unit draws, unless the
        // interface is not drawn.
        if (!bare && (model.unit->flags & OA_UNIT_FLAG_SELECTED) != 0 &&
            !model.instance->model().objects.empty())
            oa::present::world_renderer::overlay_selection_box(
                world_record.game,
                selection_raster,
                &models.bridge.surface,
                *model.unit,
                model.instance->model().objects.front()
            );
        model_render::note_piece_changes(model);
        add_model(model, model_region(model, unit_level), idle, unit_level, -1);
    };
    // A record's shatter fragment, drawn into
    // the model bridge and written back before the record's sprite.
    const auto plan_fragment = [&](const oa::sim::effect_particles::ParticleDraw& item) {
        // A frame of dots draws no model.
        if (dots)
            return;
        const auto& fragment = *item.fragment;
        const auto& look = fragment.look;
        if (look.model == nullptr)
            return;
        const auto* prepared =
            prepared_type_model(models.library, loaded_commander_types_, *look.model);
        if (prepared == nullptr || look.object >= prepared->objects.size() ||
            look.primitive >= prepared->objects[look.object].primitives.size())
            return;
        // Part of the way through the ticks' flight and spin.
        const FixedVec3 at =
            point_along_step(item.position, item.motion, presentation.batch, presentation.fraction);
        FragmentDraw drawn{};
        drawn.spin = {item.spin[0], item.spin[1], item.spin[2]};
        if (between_ticks)
            drawn.spin = {
                angle_along_step(
                    item.spin[0], item.spin_motion[0], presentation.batch, presentation.fraction
                ),
                angle_along_step(
                    item.spin[1], item.spin_motion[1], presentation.batch, presentation.fraction
                ),
                angle_along_step(
                    item.spin[2], item.spin_motion[2], presentation.batch, presentation.fraction
                )
            };
        drawn.position = {
            wrapping_sub(at.x, camera_fixed(renderer.camera_x)),
            at.y,
            wrapping_sub(at.z, camera_fixed(renderer.camera_y))
        };
        const int32_t x = high_word(drawn.position.x) + renderer.origin_x;
        const int32_t y =
            high_word(drawn.position.z) - (high_word(drawn.position.y) >> 1) + renderer.origin_y;
        // A turned point stays within the sum of its magnitudes.
        int32_t reach = 0;
        for (const auto& point : fragment.points)
            reach = std::max(
                reach,
                std::abs(high_word(point.x)) + std::abs(high_word(point.y)) +
                    std::abs(high_word(point.z))
            );
        reach += kModelRegionMargin;
        drawn.region = {x - reach, y - reach, x + reach, y + reach};
        drawn.fragment = &fragment;
        drawn.primitive = &prepared->objects[look.object].primitives[look.primitive];
        draw_list.fragments.push_back(drawn);
        add_draw(WorldDrawKind::fragment, draw_list.fragments.size() - 1);
    };
    // The explosions' flashes light the frame as strongly as the player's
    // Explosion flash setting asks, held lower where the mod's profile
    // designs them lower (ui.explosion-flash); off, none is planned, and
    // none without the match's light table.
    const auto flash_level = view_rules::explosion_flash_drawn(ui_rules(), explosion_flash());
    const bool flashes_drawn = flash_level != oa::ui::engine_settings::ExplosionFlash::off &&
                               display_.context.light_table != nullptr;
    draw_list.flash_strength = flash_level == oa::ui::engine_settings::ExplosionFlash::reduced
                                   ? FlashStrength::reduced
                                   : FlashStrength::full;
    // Explosion records and the effect layers. An explosion record's flash
    // and sprite are raised by half the record's whole height rounded down,
    // as draw_explosions culls the record and the game places them, so an
    // odd height does not lift them a row; the layers' items are raised to
    // the nearest pixel.
    // Resolve only the installed EMG impact art; unrelated explosions keep their presentation.
    std::vector<const oa::formats::gaf::Sequence*> emg_impacts;
    if (full_presentation() || live_renderer_active()) {
        for (uint32_t index = 0; index < oa::world_weapon_def_count(&world_record); ++index) {
            const oa::WeaponDef* definition =
                oa::world_weapon_def(&world_record, oa::oa_ref_from_index(index));
            if (definition == nullptr || !renderer_weapon_style::is_emg(*definition))
                continue;
            oa::Projectile reference{};
            reference.def = oa::oa_ref_from_index(index);
            const auto* weapon = match_->projectile_weapon(reference);
            if (weapon == nullptr)
                continue;
            const auto* sequence = explosion_sequence(weapon->explosion_gaf, weapon->explosion_art);
            if (sequence &&
                std::find(emg_impacts.begin(), emg_impacts.end(), sequence) == emg_impacts.end())
                emg_impacts.push_back(sequence);
        }
    }
    const auto* light_smoke = gaf_sequence(match_fx_, "smoke 1");
    const auto* dark_smoke = gaf_sequence(match_fx_, "smoke 2");
    auto effect_lift = HeightLift::nearest;
    bool explosion_pass = false;
    uint16_t effect_layer = 0;
    auto plan_effect = [&](const oa::sim::effect_particles::ParticleDraw& item) {
        if (item.kind == oa::sim::effect_particles::DrawKind::fragment) {
            plan_fragment(item);
            return;
        }
        const std::array<uint32_t, 3> at{
            static_cast<uint32_t>(item.position.x),
            static_cast<uint32_t>(item.position.y),
            static_cast<uint32_t>(item.position.z)
        };
        try {
            if (item.sight_gated && !match_->point_shown(match_view_player(), at))
                return;
        } catch (const std::exception&) {
            return;
        }
        // Part of the way through the ticks' steps, on the scene's map
        // pixels.
        const auto shown_position =
            point_along_step(item.position, item.motion, presentation.batch, presentation.fraction);
        const auto place = place_world_point(
            scene_view, oa::sim::match_runtime::fixed_words(shown_position), effect_lift
        );
        const auto pal = static_cast<std::size_t>(item.color) * 4U;
        if (item.kind == oa::sim::effect_particles::DrawKind::pixel) {
            const std::array<uint8_t, 3> color{
                match_palette_[pal], match_palette_[pal + 1], match_palette_[pal + 2]
            };
            draw_list.spatial_particles.push_back(
                {shown_position, color, effect_layer == oa::sim::effect_particles::layer_nano}
            );
            // The square covers the scene pixels of its map pixels, from
            // its top left corner at the point, and one at least; clipped to
            // the frame before it is filled.
            const uint32_t step = oa::present::scene_step(scene_view.scale);
            const auto span = [step](int32_t from, uint32_t into) {
                const int64_t first = oa::present::first_scene_pixel(from, step, into % step);
                const int64_t end = oa::present::first_scene_pixel(
                    int64_t{from} + oa::sim::effect_particles::pixel_item_side, step, into % step
                );
                return std::pair<int64_t, int64_t>{first, std::max(end, first + 1)};
            };
            const auto [left, right] = span(place.x, phase[0]);
            const auto [top, bottom] = span(place.y, phase[1]);
            draw_list.squares.push_back(
                {static_cast<int32_t>(std::clamp<int64_t>(left, 0, world_surface.width)),
                 static_cast<int32_t>(std::clamp<int64_t>(top, 0, world_surface.height)),
                 static_cast<int32_t>(std::clamp<int64_t>(right, 0, world_surface.width)),
                 static_cast<int32_t>(std::clamp<int64_t>(bottom, 0, world_surface.height)),
                 color}
            );
            add_draw(WorldDrawKind::pixel_square, draw_list.squares.size() - 1);
            return;
        }
        const bool flash = item.kind == oa::sim::effect_particles::DrawKind::flash;
        if ((item.kind != oa::sim::effect_particles::DrawKind::sprite && !flash) ||
            (flash && !flashes_drawn) || item.sequence == nullptr || item.frame < 0 ||
            static_cast<std::size_t>(item.frame) >= item.sequence->frames.size())
            return;
        const auto* decoded =
            effect_frame(draw_list, *item.sequence, static_cast<std::size_t>(item.frame));
        if (decoded == nullptr)
            return;
        draw_list.sprites.push_back({decoded, place});
        draw_list.sprites.back().world_position = shown_position;
        draw_list.sprites.back().surface_effect = explosion_pass && !flash;
        if (!flash && emg_impact_at(item.position) &&
            std::find(emg_impacts.begin(), emg_impacts.end(), item.sequence) != emg_impacts.end()) {
            auto& sprite = draw_list.sprites.back();
            constexpr float impact_scale = 0.6F;
            constexpr float impact_opacity = 0.72F;
            const float age =
                float(item.frame) / std::max<std::size_t>(1, item.sequence->frames.size() - 1);
            sprite.effect_scale = impact_scale;
            sprite.effect_opacity = impact_opacity * (1.0F - 0.65F * age);
            sprite.soft_effect = true;
        }
        if ((full_presentation() || live_renderer_active()) && !flash &&
            (item.sequence == light_smoke || item.sequence == dark_smoke)) {
            auto& sprite = draw_list.sprites.back();
            sprite.soft_effect = true;
            if (emg_smoke_at(item.position)) {
                // Sustained rapid-fire hits keep a readable target silhouette through local smoke.
                constexpr float smoke_opacity = 0.35F;
                sprite.effect_opacity = smoke_opacity;
            }
        }
        add_draw(
            flash ? WorldDrawKind::lit_sprite : WorldDrawKind::sprite, draw_list.sprites.size() - 1
        );
    };
    const auto visit = [](void* context, const oa::sim::effect_particles::ParticleDraw& item) {
        (*static_cast<decltype(plan_effect)*>(context))(item);
    };
    const auto plan_effect_layers = [&](uint16_t first, uint16_t last) {
        plan_commit();
        for (uint16_t layer = first; layer <= last; ++layer) {
            effect_layer = layer;
            oa::sim::effect_particles::draw_layer(match_->effects(), layer, &plan_effect, visit);
        }
        effect_layer = 0;
    };

    // The features and units this frame draws, placed in the draw order: a
    // feature by the plot holding it, a unit by its depth.
    struct FeatureToDraw {
        MatchFeatureDraw* object{};
        const MatchGafFeatureDraw* sprite{};
        oa::present::model::FeatureDraw plan{};
        /// Where a sprite feature stands on the scene's map pixels
        /// (place_world_point).
        oa::present::world_renderer::ScreenPoint place{};
    };

    const auto feature_height = [&](uint16_t feature_index) -> int8_t {
        return feature_index < world_record.feature_def_count
                   ? world_record.feature_defs[feature_index].height
                   : int8_t{0};
    };
    std::vector<FeatureToDraw> features_to_draw;
    std::vector<oa::present::world_renderer::FeatureDrawSite> feature_sites;
    // Room for every feature of both kinds, so that the lists do not grow
    // as a frame fills them.
    features_to_draw.reserve(match_features_.size() + match_gaf_features_.size());
    feature_sites.reserve(match_features_.size() + match_gaf_features_.size());
    // A feature off the battlefield is passed over before its fog and its
    // frames are looked at.
    for (auto& feature : match_features_) {
        // A sinking wreck's record moves after the draw was placed. The
        // picture follows it, and stays where the record rests.
        follow_feature_draw(feature, world_record);
        const auto feature_screen = project_match_point(
            viewport,
            {static_cast<uint32_t>(feature.position.x),
             static_cast<uint32_t>(feature.position.y),
             static_cast<uint32_t>(feature.position.z)}
        );
        if (!on_battlefield(feature_screen.x, feature_screen.y) ||
            feature_hidden_by_fog(feature.feature_index, feature.cell_x, feature.cell_z))
            continue;
        features_to_draw.push_back({&feature, nullptr, {}, {}});
        feature_sites.push_back(
            {feature.cell_x, feature.cell_z, feature_height(feature.feature_index)}
        );
    }
    // A sprite feature is culled on the battlefield and drawn over the map
    // pixels it stands on, as the scene lays them at its scale.
    for (const auto& feature : match_gaf_features_) {
        const std::array<uint32_t, 3> position{
            static_cast<uint32_t>(feature.position.x),
            static_cast<uint32_t>(feature.position.y),
            static_cast<uint32_t>(feature.position.z)
        };
        const auto screen = project_match_point(viewport, position);
        if (!on_battlefield(screen.x, screen.y) ||
            feature_hidden_by_fog(feature.feature_index, feature.cell_x, feature.cell_z))
            continue;
        const auto plan =
            oa::present::model::plan_feature_draw(world_record, feature.cell_x, feature.cell_z);
        if (plan.object)
            continue;
        features_to_draw.push_back(
            {nullptr, &feature, plan, place_world_point(scene_view, position, HeightLift::nearest)}
        );
        feature_sites.push_back(
            {feature.cell_x, feature.cell_z, feature_height(feature.feature_index)}
        );
    }
    std::vector<uint16_t> units_to_draw;
    std::vector<oa::present::world_renderer::UnitDrawSite> unit_sites;
    for (auto& slot : match_->world().slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr || slot.record.type_index == 0)
            continue;
        try {
            if (!match_->unit_shown(static_cast<uint8_t>(match_view_player()), slot.unit_index))
                continue;
        } catch (const std::exception&) {
            continue;
        }
        // A unit drawn on its playout is kept and ordered where it is drawn;
        // whether it is seen stays with its simulated place.
        const auto pose = mirrored_pose(models, world_record, slot.unit_index, moment);
        const auto unit_screen = project_match_point(
            viewport,
            pose ? oa::sim::match_runtime::fixed_words(pose->position) : slot.unit->position
        );
        if (!on_battlefield(unit_screen.x, unit_screen.y))
            continue;
        const auto model = unit_model(models, slot.unit_index);
        if (model.instance == nullptr)
            continue;
        units_to_draw.push_back(slot.unit_index);
        unit_sites.push_back(
            {pose ? pose->position.z : model.unit->position.z,
             model.unit->flags,
             pose ? pose->position.x : model.unit->position.x,
             model.unit->footprint_x,
             model.unit->footprint_z}
        );
    }
    oa::present::world_renderer::BattlefieldDrawPlan draw_plan;
    oa::present::world_renderer::plan_battlefield_draws(
        feature_sites, unit_sites, renderer.camera_y, draw_plan
    );
    const auto plan_feature = [&](uint32_t index) {
        auto& feature = features_to_draw[index];
        if (feature.object != nullptr)
            plan_object_feature(*feature.object);
        else
            plan_sprite_feature(*feature.sprite, feature.plan, feature.place);
    };
    // The battlefield draws far to near: effect layers 0 to 2 (wakes among
    // them), the lying features, layers 3 and 4, then row by row the ground
    // units and the standing features, layers 5 and 6, the projectiles,
    // debris and explosions, layer 7, the units off the ground, then layers 8
    // and 9 (smoke); the fog goes over them all, and the order overlays over
    // the fog.
    plan_effect_layers(0, 2);
    for (const auto index : draw_plan.lying_features)
        plan_feature(index);
    plan_effect_layers(3, 4);
    for (const auto& draw : draw_plan.ground) {
        if (draw.kind == oa::present::world_renderer::DrawnThing::unit)
            plan_unit(units_to_draw[draw.index]);
        else
            plan_feature(draw.index);
    }
    plan_effect_layers(5, 6);
    // The battlefield rectangle the load screen sets, over the visible map
    // area, which the projectiles' lenses and the explosion records are
    // culled to.
    const oa::sim::effect_particles::ExplosionView explosion_view{
        renderer.camera_x,
        renderer.camera_y,
        {oa::sim::effect_particles::battlefield_screen_x,
         oa::sim::effect_particles::battlefield_screen_y,
         oa::sim::effect_particles::battlefield_screen_x + vis_w - 1,
         oa::sim::effect_particles::battlefield_screen_y + vis_h - 1}
    };
    if (!dots)
        plan_projectile_models(models);
    // Every captured tile goes back to the frame after the projectiles.
    add_draw(WorldDrawKind::commit_always, 0);
    plan_match_projectiles(draw_list, scene_view, explosion_view);
    // Nano streams are the type-6 particles; the beam line is a debug aid.
    const auto debug_beams = options_.debug_order_lines
                                 ? match_->nano_lasers()
                                 : std::vector<oa::sim::match_runtime::Match::NanoLaser>{};
    for (const auto& beam : debug_beams) {
        const auto from = project_match_point(scene_view, beam.from);
        const std::array<uint32_t, 3> dest{
            static_cast<uint32_t>(beam.to_origin[0] + beam.to_extent[0] / 2),
            static_cast<uint32_t>(beam.to_origin[1] + beam.to_extent[1] / 2),
            static_cast<uint32_t>(beam.to_origin[2] + beam.to_extent[2] / 2)
        };
        const auto to = project_match_point(scene_view, dest);
        const auto pal = static_cast<std::size_t>(0xa1) * 4U;
        const std::array<uint8_t, 3> color =
            pal + 2 < match_palette_.size()
                ? std::array<
                      uint8_t,
                      3>{match_palette_[pal], match_palette_[pal + 1], match_palette_[pal + 2]}
                : std::array<uint8_t, 3>{80, 255, 80};
        draw_list.lines.push_back({from.x, from.y, to.x, to.y, color, 0});
        add_draw(WorldDrawKind::line, draw_list.lines.size() - 1);
    }
    // The in-bounds pass starts with the debris pieces: each piece's object is turned
    // by its spin and drawn at its origin, culled on
    // that origin, in its unit's team colour.
    const auto& debris_table = match_->effects().debris;
    auto plan_debris_piece = [&](const oa::sim::effect_particles::DebrisPiece& live) {
        if (dots)
            return;
        // Part of the way through the tick's fall and spin, by the piece's slot.
        const auto slot = static_cast<std::size_t>(&live - std::begin(debris_table));
        const auto piece =
            presented_debris(presentation.debris, renderer.tick, slot, live, presentation.fraction);
        if (piece.model == nullptr || piece.object >= piece.model->objects.size())
            return;
        const auto& object = piece.model->objects[piece.object];
        const auto* prepared =
            prepared_type_model(models.library, loaded_commander_types_, *piece.model);
        if (prepared == nullptr)
            return;
        const auto* owner = oa::world_unit_at(&world_record, piece.unit);
        DebrisDraw drawn{};
        drawn.team = renderer.team_colors[(owner != nullptr ? owner->owner_index : 0) % 10];
        const int32_t x =
            high_word(wrapping_sub(piece.position.x, camera_fixed(renderer.camera_x))) +
            renderer.origin_x;
        const int32_t y =
            high_word(wrapping_sub(piece.position.z, camera_fixed(renderer.camera_y))) -
            (high_word(piece.position.y) >> 1) + renderer.origin_y;
        // A turned point stays within the sum of its magnitudes.
        int32_t reach = 0;
        for (const auto& vertex : object.vertices)
            reach = std::max(
                reach,
                std::abs(high_word(vertex.x)) + std::abs(high_word(vertex.y)) +
                    std::abs(high_word(vertex.z))
            );
        reach += kModelRegionMargin;
        drawn.region = {x - reach, y - reach, x + reach, y + reach};
        drawn.object = &object;
        drawn.prepared = &prepared->objects[piece.object];
        drawn.spin = {piece.spin[2], piece.spin[1], piece.spin[0]};
        drawn.origin = {piece.position.x, piece.position.y, piece.position.z};
        draw_list.debris.push_back(drawn);
        add_draw(WorldDrawKind::debris, draw_list.debris.size() - 1);
    };
    // The pieces' particles start in the match's tick, never here.
    oa::sim::effect_particles::draw_debris(
        match_->effects(),
        &plan_debris_piece,
        [](void* context, const oa::sim::effect_particles::DebrisPiece& piece) {
            (*static_cast<decltype(plan_debris_piece)*>(context))(piece);
        }
    );
    effect_lift = HeightLift::down;
    explosion_pass = true;
    oa::sim::effect_particles::draw_explosions(
        match_->effects(), explosion_view, &plan_effect, visit
    );
    effect_lift = HeightLift::nearest;
    explosion_pass = false;
    plan_effect_layers(7, 7);
    for (const auto index : draw_plan.raised_units)
        plan_unit(units_to_draw[index]);
    // The building being placed, as the profile's build preview shows it.
    if (!bare && !dots && ready_build_preview(models)) {
        auto& preview = models.build_preview;
        const model_render::ModelRef model{
            &preview.instance,
            &model_render::prepare_model(models.library, preview.instance.model_handle()),
            &preview.state,
            &preview.unit,
            oa::world_unit_def(&world_record, preview.unit.def)
        };
        model_render::note_piece_changes(model);
        model_render::update_model_transforms(model);
        // Its look at this tick of its pulse, which starts again as the
        // player turns it, at every zoom and however often frames are drawn.
        constexpr uint32_t pulse = view_rules::build_preview_pulse_ticks;
        const uint32_t pulse_tick = (renderer.tick % pulse + pulse - build_turn_phase_) % pulse;
        preview.state.build_bands =
            build_preview_bands(model, pulse_tick, ui_rules().build_preview.fill);
        draw_list.stand_ins.push_back(preview.unit);
        add_model(
            model,
            model_region(model, model_render::UnitSupersampling::off),
            false,
            model_render::UnitSupersampling::off,
            static_cast<int32_t>(draw_list.stand_ins.size() - 1)
        );
    }
    plan_effect_layers(8, 8);
    plan_effect_layers(9, 9);
    // Every band of the frame draws the list: on the drawing threads at
    // once, or in order on this one.
    std::ignore = collect_match_effect_sources();
    WorldFrameDraw frame_draw{};
    frame_draw.target = {
        world_surface.rgb.data(),
        static_cast<int32_t>(world_surface.width),
        static_cast<int32_t>(world_surface.height),
        world_pixel_clip_.x,
        world_pixel_clip_.y,
        world_pixel_clip_.w,
        world_pixel_clip_.h,
        0,
        static_cast<int32_t>(world_surface.height),
        phase[0],
        phase[1]
    };
    {
        // The scene's pixels that show the map, as its terrain was filled.
        const auto across = oa::present::world_renderer::shown_map_span(
            camera_x, static_cast<uint32_t>(map_width), draw_scale, phase[0]
        );
        const auto down = oa::present::world_renderer::shown_map_span(
            camera_y, static_cast<uint32_t>(map_height), draw_scale, phase[1]
        );
        frame_draw.shown_map = {across.first, down.first, across.end, down.end};
    }
    frame_draw.palette = &match_palette_;
    frame_draw.scale = scene_view.scale;
    frame_draw.bridge = &models.bridge;
    frame_draw.display = &models.display;
    frame_draw.light_table = display_.context.light_table;
    frame_draw.projectile_shadow = &models.projectile_shadow;
    frame_draw.debris_view = {0, 0, vis_w - 1, vis_h - 1};
    // A scene the area pass reduces draws its thin lines about one screen
    // pixel thick; every other frame draws them as the game always has.
    if (scaling.method == SceneMethod::area) {
        frame_draw.line_thickness =
            oa::present::world_renderer::scene_line_thickness(draw_scale, match_zoom());
        frame_draw.bridge_line_thickness =
            oa::present::world_renderer::scene_line_thickness(1.0F, match_zoom());
    }
    // The card draws every kind of the list in the Full tier; the bands
    // draw nothing there.
    if (!card_world && !live_world)
        draw_world_bands(models, frame_draw, draw_pool_.get());
    oa::present::bind_display(bound_display);
    mark_profile(OA_PROFILE_RENDER_STUFF);
    const auto fog_pass = [&](FogPasses passes) {
        apply_match_fog(
            world_surface,
            camera_x,
            camera_y,
            scene_view.destination_x,
            scene_view.destination_y,
            scene_w,
            scene_h,
            draw_scale,
            passes
        );
    };
    // The dithered fog clears every other pixel of the scene. Where the card
    // enlarges the scene, or it is resampled nearest at another scale, those
    // pixels would grow into blocks or break into a pattern that moves with
    // the zoom, so the fog draws the even tone the dither averages to.
    fog_shading_.dither_even =
        scaling.method == SceneMethod::magnify ||
        (scaling.method == SceneMethod::nearest && draw_scale != match_zoom());
    if (!live_world)
        fog_pass(FogPasses::both);
    // The dots over the fog, as the minimap shows its units over its own.
    if (dots)
        draw_far_view_dots(
            world_surface.rgb.data(),
            static_cast<int32_t>(world_surface.width),
            static_cast<int32_t>(world_surface.height),
            far_dots
        );
    // Magnified scenes must carry their effects through the same camera shift
    // and sampling as their terrain, rather than becoming fixed opaque overlays.
    if (!card_world && !live_renderer_active() && scaling.method == SceneMethod::magnify) {
        const auto place = match_view_place();
        const float zoom = match_zoom();
        compose_match_effect_pixels(
            world_surface.rgb,
            world_surface.width,
            world_surface.height,
            world_surface.width * 3,
            3,
            static_cast<float>((camera_x - place[0]) * zoom),
            static_cast<float>((camera_y - place[1]) * zoom),
            draw_scale / zoom,
            draw_scale / zoom
        );
    }
    // A scene drawn apart becomes the world layer's picture at the zoom, on
    // which everything after the fog is drawn in screen pixels.
    if (scaling.apart) {
        match_world_cpu_.width = static_cast<uint32_t>(bf_w);
        match_world_cpu_.height = static_cast<uint32_t>(bf_h);
        match_world_cpu_.rgb.resize(
            static_cast<std::size_t>(bf_w) * static_cast<std::size_t>(bf_h) * 3U
        );
        const oa::present::world_renderer::RgbSource scene{
            world_surface.rgb.data(), world_surface.width, world_surface.height, world_surface.width
        };
        const oa::present::world_renderer::RgbTarget picture{
            match_world_cpu_.rgb.data(),
            match_world_cpu_.width,
            match_world_cpu_.height,
            match_world_cpu_.width
        };
        // The passes a scene drawn apart adds are timed for the step-down.
        if (scaling.method == SceneMethod::area)
            area_filter_scene(world_surface);
        else if (
            const auto error = oa::present::world_renderer::resample_nearest_rgb24(
                scene,
                static_cast<float>(static_cast<double>(match_zoom()) / draw_scale),
                picture,
                draw_pool_.get()
            );
            error != oa::present::world_renderer::AreaError::none
        )
            throw std::runtime_error(
                std::string("cannot resample the battlefield's scene: ") +
                oa::present::world_renderer::area_error_text(error)
            );
        // The card magnifies the scene itself; what the painters change on
        // the nearest picture is found against it, the base.
        if (scaling.method == SceneMethod::magnify)
            accelerated_.base.assign(match_world_cpu_.rgb.begin(), match_world_cpu_.rgb.end());
        match_scene_cpu_ = {
            world_surface.width, world_surface.height, std::move(world_surface.rgb)
        };
        world_surface = {
            match_world_cpu_.width, match_world_cpu_.height, std::move(match_world_cpu_.rgb)
        };
        world_pixel_clip_ = {0, 0, bf_w, bf_h};
    }
    if (!card_world && !live_renderer_active() && scaling.method != SceneMethod::magnify)
        compose_match_effect_pixels(
            world_surface.rgb,
            world_surface.width,
            world_surface.height,
            world_surface.width * 3,
            3,
            0,
            0,
            1,
            1
        );
    // What the painters after the fog draw at places on the map goes where
    // the view shows those places: a view drawn between map pixels moves it
    // by the view's offset, to the nearest screen pixel.
    auto painted = viewport;
    align_order_overlay_viewport(painted);
    // Over the fog tiles: the local player's order overlays while the queue
    // modifier is held (asked of that role, not the pointer word), so that orders
    // queued onto never-mapped or unseen ground stay in view (in 3.1c the
    // smoke and the fog cover them), then the HUD overlay's outline of the
    // build site or the drag box. The fog's profile category takes the time
    // of both.
    if (!bare) {
        if (!live_renderer_active() &&
            (modifier_role_active("order.queue-modifier") || virtual_shift(ModifierUse::order)))
            // What the pass drew is for the checks alone.
            std::ignore = draw_order_overlays(world_surface, painted);
        if (build_queue_drag_ && build_queue_drag_->moved)
            draw_build_queue_drag(world_surface, painted);
        else
            draw_build_ghost(world_surface, painted);
        draw_selection_band(world_surface, painted);
    }
    mark_profile(OA_PROFILE_RENDER_FOG);
    match_world_cpu_ = {world_surface.width, world_surface.height, std::move(world_surface.rgb)};
    // Without the interface the frame is the world alone: no panel, bar,
    // label, message, board or overlay is painted over it.
    drawn_health_bars_.clear();
    if (bare) {
        // A turned director frame is the rotated view's picture, drawn here
        // as the frame with the interface draws it below; one the card
        // cannot draw ends the frame, as there.
        const bool drawn = !live_world || render_live_match_surface();
        overlay_target_ = nullptr;
        hud_source_space_ = false;
        paint_origin_ = {};
        return drawn;
    }
    // The HUD overlay: resource bars, bottom panel, then radar at
    // Game.radar_offset_x and radar_offset_y, then the GUI's child gadgets last.
    paint_on(PaintLayer::hud);
    blit_match_minimap();
    draw_shared_camera_rectangles();
    // Nothing selected leaves the side's idle page in the well: its panel
    // art is the emblem under the minimap, already on the HUD.
    paint_on(PaintLayer::battlefield);
    // The bars shrink as the view zooms out, and keep the game's size at
    // zoom 1 and in.
    const auto bar_size = oa::ui::hud::health_bar_size(match_zoom());
    // The digits lie over every unit's bar and over each other's boxes.
    std::optional<BackedText> unit_digits(std::in_place, *this);
    for (const auto& slot : match_->world().slots) {
        if (live_renderer_active())
            break;
        if (slot.unit_index == 0 || slot.unit == nullptr || slot.record.type_index == 0 ||
            slot.unit->type == nullptr)
            continue;
        try {
            if (slot.owner_index != match_view_player() &&
                !match_->unit_shown(static_cast<uint8_t>(match_view_player()), slot.unit_index))
                continue;
        } catch (const std::exception&) {
            continue;
        }
        // The bars and digits stay with the unit where it shows.
        const bool shown_elsewhere =
            between_ticks ||
            mirrored_pose(models, world_record, slot.unit_index, moment).has_value();
        const auto screen = shown_elsewhere
                                ? project_match_point(
                                      painted,
                                      oa::sim::match_runtime::fixed_words(
                                          shown_unit_position(models, world_record, slot.unit_index)
                                      )
                                  )
                                : project_match_point(painted, slot.unit->position);
        const int bar_x = screen.x;
        const int bar_y = screen.y + bar_size.below_unit;
        // Dots carry no health bar or squad digit, which would hide the
        // dots around them.
        if (const auto& world = match_->state();
            !dots && oa::ui::hud::draws_health_bar(world, slot.record)) {
            const auto* def = oa::world_unit_def_of(&world, &slot.record);
            oa::ui::hud::HealthBar bar{};
            if (def != nullptr && oa::ui::hud::unit_health_bar(
                                      world.game, slot.record, *def, bar_x, bar_y, bar, bar_size
                                  )) {
                const auto fill = [this](const oa::Rect32& rect, uint8_t color) {
                    fill_hud_rect(
                        rect.x1, rect.y1, rect.x2 - rect.x1 + 1, rect.y2 - rect.y1 + 1, color
                    );
                };
                fill(bar.trough, bar.trough_color);
                fill(bar.fill, bar.fill_color);
                drawn_health_bars_.push_back(bar);
            }
        }
        // The squad digit of the viewpoint player's own unit in a squad,
        // two rows below its bar, and the self-destruct count above it. The
        // digits under and over the bar keep the game font's size at most,
        // so that they stay clear of the bar.
        const PanelText unit_labels(*this);
        if (!dots && oa::ui::hud::draws_squad_digit(match_->state(), slot.record))
            draw_match_label(
                bar_x - 4,
                bar_y + bar_size.half_height + 2,
                std::string(1, oa::ui::hud::squad_digit(slot.record)),
                255
            );
        if (const auto count = match_->self_destruct_remaining(slot.unit_index); count != 0)
            draw_match_label(
                bar_x - 4, bar_y - bar_size.half_height - 10, std::to_string(count), 1
            );
    }
    unit_digits.reset();
    // The rotated view replaces the battlefield's picture, so it is drawn
    // before what lies over the battlefield: the megamap, the markers and
    // the commander placement. A rotated frame the card cannot draw ends the
    // frame here, before anything is drawn over the battlefield it left out.
    if (!render_live_match_surface()) {
        overlay_target_ = nullptr;
        hud_source_space_ = false;
        paint_origin_ = {};
        return false;
    }
    // ui.megamap draws over the battlefield in its place.
    draw_megamap();
    paint_on(PaintLayer::hud);
    {
        // The bars' text keeps to the game fonts' size at most; the chat
        // line follows the text size.
        const PanelText panel(*this);
        {
            // The bars' lines lie over every box laid among them: the
            // unit's status over the rate figures beside it, the figures
            // and the clock line over each other.
            const BackedText bars(*this);
            // The unit panel shows the unit under the cursor alone, never
            // the selection.
            draw_unit_panel();
            draw_resource_readout();
            draw_clock_line(PaintLayer::hud);
            draw_build_captions();
        }
        draw_key_badges();
        call_hook_or_report<&Extension::draw_match_hud>(extension_, hook_error_report(), *this);
    }
    draw_chat_entry();
    paint_on(PaintLayer::battlefield);
    // The markers' map points are placed as the classic view draws them, so
    // the rotated view leaves them out rather than drawing them elsewhere.
    // Each overlay's lines lie over every box laid among them (BackedText).
    if (!live_renderer_active()) {
        const BackedText markers(*this);
        draw_whiteboard(painted);
    }
    draw_middle_pan_marks();
    {
        const BackedText prompt(*this);
        draw_commander_placement();
    }
    {
        const BackedText board(*this);
        draw_match_kill_board();
    }
    // Then the status strip over the bottom of the battlefield. The panels'
    // text keeps to the game fonts' size at most (PanelText).
    {
        const PanelText panel(*this);
        const BackedText strip(*this);
        draw_status_panel();
    }
    {
        const BackedText rows(*this);
        draw_resource_panel_overlay();
    }
    // The clock line, the game clock and the message log share the
    // battlefield's top left: their lines lie over each other's boxes and
    // the log's backdrops alike.
    {
        const BackedText lines(*this);
        draw_clock_line(PaintLayer::battlefield);
        draw_console_clock();
        draw_match_message_log();
    }
    {
        const BackedText debug_line(*this);
        draw_debug_status_line();
    }
    {
        const BackedText stats(*this);
        draw_frame_stats();
    }
    draw_key_latch_chips();
    draw_hold_ring();
    draw_extension_overlay();
    draw_risen_chat_line();
    draw_unit_info_panel();
    draw_profile_bars();
    // The outcome, the paused title (a menu's hold or the pause bit) and the
    // menus over the battlefield.
    draw_end_overlay();
    outcome_frame_drawn_ = match_finished_;
    overlay_target_ = nullptr;
    hud_source_space_ = false;
    paint_origin_ = {};
    // Director mode composes its own frame (draw_director_frame).
    if (!match_use_layers_ && !directed)
        compose_match_frame(surface_);
    return true;
}

void Runtime::compose_shown_match(renderer::Surface& frame) {
    // The card drew the battlefield, or magnified the processor's scene,
    // into a picture the world layer does not hold: that picture is read
    // back. Any other frame presented the world layer as it stands, the
    // area pass's picture below zoom 1 among them.
    const bool card_world = full_frame_drawn();
    const bool magnified = !card_world && !full_presentation() && !live_renderer_active() &&
                           accelerated_presentation() &&
                           accelerated_.frame.method == SceneMethod::magnify &&
                           accelerated_.magnified;
    if (!card_world && !magnified) {
        compose_match_layers(frame);
        return;
    }
    renderer::Surface presented;
    if (!read_presented_world(presented)) {
        ensure_screen_world();
        compose_match_layers(frame);
        return;
    }

    // The world layer stays the frame's own: the picture stands in for it
    // while the canvas is composed.
    struct WorldBack {
        renderer::Surface& world;
        renderer::Surface& kept;

        WorldBack(renderer::Surface& layer, renderer::Surface& picture)
            : world(layer), kept(picture) {
            std::swap(world, kept);
        }

        WorldBack(const WorldBack&) = delete;
        WorldBack& operator=(const WorldBack&) = delete;

        ~WorldBack() { std::swap(world, kept); }
    } world_back{match_world_cpu_, presented};

    compose_match_layers(frame);
}

bool Runtime::read_presented_world(renderer::Surface& world) {
    if (sdl_.renderer == nullptr || screen_ != Screen::match || !match_ || !match_use_layers_ ||
        match_hud_cpu_.rgb.empty() || match_world_cpu_.rgb.empty() ||
        (render_run_ && render_run_->device_lost))
        return false;
    const uint32_t bf_w = match_world_cpu_.width;
    const uint32_t bf_h = match_world_cpu_.height;
    if (bf_w == 0 || bf_h == 0)
        return false;

    // The frame drawn again as its tier presents it, from the layers and the
    // card's state the last frame left, into the window's frame, which the
    // next frame clears; finish_match_layers neither draws over it nor
    // presents it.
    struct ReadingBack {
        bool& flag;

        explicit ReadingBack(bool& reading) : flag(reading) { flag = true; }

        ReadingBack(const ReadingBack&) = delete;
        ReadingBack& operator=(const ReadingBack&) = delete;

        ~ReadingBack() { flag = false; }
    } reading_back{accelerated_.reading_back};

    try {
        if (full_frame_drawn()) {
            if (!full_presentation() || !present_full_match_layers(false))
                return false;
        } else {
            present_accelerated_match_layers(false);
            if (!accelerated_.magnified)
                return false;
        }
    } catch (const AccelerationError&) {
        // The next frame meets the failure again and handles it.
        return false;
    } catch (const PresentError&) {
        return false;
    }

    // The battlefield's place in the window's own pixels, through the
    // logical presentation and the scale the match is drawn with.
    SDL_Renderer* drawn_on = sdl_.renderer;
    int logical_width = 0;
    int logical_height = 0;
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_FRect shown{};
    float scale_x = 1.0F;
    float scale_y = 1.0F;
    if (!SDL_GetRenderLogicalPresentation(drawn_on, &logical_width, &logical_height, &mode) ||
        !SDL_GetRenderLogicalPresentationRect(drawn_on, &shown) ||
        !SDL_GetRenderScale(drawn_on, &scale_x, &scale_y))
        return false;
    if (mode != SDL_LOGICAL_PRESENTATION_DISABLED && logical_width > 0 && logical_height > 0) {
        scale_x *= shown.w / static_cast<float>(logical_width);
        scale_y *= shown.h / static_cast<float>(logical_height);
    } else {
        shown.x = 0.0F;
        shown.y = 0.0F;
    }
    const SDL_Rect area{
        static_cast<int>(std::lround(shown.x + static_cast<float>(match_layout_.left) * scale_x)),
        static_cast<int>(std::lround(shown.y + static_cast<float>(match_layout_.top) * scale_y)),
        std::max(1, static_cast<int>(std::lround(static_cast<float>(bf_w) * scale_x))),
        std::max(1, static_cast<int>(std::lround(static_cast<float>(bf_h) * scale_y)))
    };
    using SurfaceOwner = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
    SurfaceOwner rgb(nullptr, SDL_DestroySurface);
    {
        const RenderState state(drawn_on);
        state.use_window_pixels();
        const SurfaceOwner read(SDL_RenderReadPixels(drawn_on, &area), SDL_DestroySurface);
        rgb.reset(read ? SDL_ConvertSurface(read.get(), SDL_PIXELFORMAT_RGB24) : nullptr);
    }
    if (!rgb || rgb->w <= 0 || rgb->h <= 0)
        return false;

    // Each layout pixel the mean of the window pixels it covers, which at
    // the layout's own scale is the one pixel under it.
    const auto read_w = static_cast<uint32_t>(rgb->w);
    const auto read_h = static_cast<uint32_t>(rgb->h);
    const auto* pixels = static_cast<const uint8_t*>(rgb->pixels);
    const auto pitch = static_cast<std::size_t>(rgb->pitch);
    renderer::Surface picture{bf_w, bf_h, std::vector<uint8_t>(std::size_t{bf_w} * bf_h * 3U)};
    for (uint32_t y = 0; y < bf_h; ++y) {
        const uint32_t top = static_cast<uint32_t>(uint64_t{y} * read_h / bf_h);
        const uint32_t bottom =
            std::max(top + 1U, static_cast<uint32_t>(uint64_t{y + 1U} * read_h / bf_h));
        for (uint32_t x = 0; x < bf_w; ++x) {
            const uint32_t left = static_cast<uint32_t>(uint64_t{x} * read_w / bf_w);
            const uint32_t right =
                std::max(left + 1U, static_cast<uint32_t>(uint64_t{x + 1U} * read_w / bf_w));
            std::array<uint32_t, 3> sum{};
            for (uint32_t row = top; row < bottom; ++row)
                for (uint32_t column = left; column < right; ++column) {
                    const uint8_t* source = pixels + row * pitch + std::size_t{column} * 3U;
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        sum[channel] += source[channel];
                }
            const uint32_t count = (bottom - top) * (right - left);
            uint8_t* target = picture.rgb.data() + (std::size_t{y} * bf_w + x) * 3U;
            for (std::size_t channel = 0; channel < 3; ++channel)
                target[channel] = static_cast<uint8_t>((sum[channel] + count / 2U) / count);
        }
    }

    // The card drew at the display gamma, which the panel's presentation
    // applies again: each level goes back to the level the gamma takes
    // nearest to it.
    if (!gamma_identity_) {
        std::array<uint8_t, 256> undone{};
        for (int shown_level = 0; shown_level < 256; ++shown_level) {
            int nearest = 0;
            int nearest_distance = std::numeric_limits<int>::max();
            for (int level = 0; level < 256; ++level) {
                const int distance = std::abs(int{gamma_table_[level]} - shown_level);
                if (distance < nearest_distance) {
                    nearest = level;
                    nearest_distance = distance;
                }
            }
            undone[shown_level] = static_cast<uint8_t>(nearest);
        }
        for (uint8_t& channel : picture.rgb)
            channel = undone[channel];
    }
    world = std::move(picture);
    return true;
}

} // namespace oa::app
