// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// World-space drawing: build ghosts, lines and GAF blits.
#include "oa/app/runtime.hpp"
#include "oa/core/map_plot.h"
#include "match_models.hpp"
#include "presentation_interpolation.hpp"
#include "world_draws.hpp"
#include "renderer_weapon_style.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

std::array<uint8_t, 3> Runtime::palette_rgb(uint8_t index) const {
    const auto pal = static_cast<std::size_t>(index) * 4U;
    if (pal + 2 >= match_palette_.size())
        return index == kPaletteGreen ? std::array<uint8_t, 3>{0, 255, 0}
                                      : std::array<uint8_t, 3>{255, 0, 0};
    return {match_palette_[pal], match_palette_[pal + 1], match_palette_[pal + 2]};
}

std::optional<Runtime::PendingBuildSite>
Runtime::pending_build_site(oa::sim::ground_orders::Point world) const {
    if (!match_ || pending_build_type_ == 0 || pending_build_type_ >= spawn_types_.size())
        return std::nullopt;
    return build_site_for(pending_build_type_, static_cast<uint8_t>(pending_build_facing()), world);
}

std::optional<Runtime::PendingBuildSite>
Runtime::build_site_for(uint16_t type, uint8_t facing, oa::sim::ground_orders::Point world) const {
    if (!match_ || type == 0 || type >= spawn_types_.size())
        return std::nullopt;
    auto fx = spawn_types_[type].footprint_x;
    auto fz = spawn_types_[type].footprint_z;
    if (fx <= 0)
        fx = 2;
    if (fz <= 0)
        fz = 2;
    // A building turned east or west lies across: its footprint's sides swap.
    facing = match_->build_facing(type, facing);
    if ((facing & 1U) != 0)
        std::swap(fx, fz);
    oa::sim::match_runtime::snap_build_position(world, fx, fz);
    const auto fx_u = static_cast<uint32_t>(fx);
    const auto fz_u = static_cast<uint32_t>(fz);
    // The footprint's first cell, rounded toward negative infinity: a
    // footprint over the left or top edge starts at a negative cell.
    const auto cell_x =
        std::bit_cast<int32_t>(static_cast<uint32_t>(world[0]) - fx_u * 0x80000u + 0x80000u) >> 20;
    const auto cell_z =
        std::bit_cast<int32_t>(static_cast<uint32_t>(world[2]) - fz_u * 0x80000u + 0x80000u) >> 20;
    // Where the rules let the build cursor place over the player's own units,
    // the cursor names unit slot 1 as the unit to skip, as the game does, so
    // that slot never refuses the site, whoever owns it.
    const uint16_t skip_unit =
        match_->rules().orders.build_site_kickout.place_over_own_units ? 1 : 0;
    // The site is tested, and stands, turned to the building's facing.
    bool over_own_units = false;
    oa::sim::match_runtime::BuildSiteOptions options{};
    options.facing = facing;
    options.over_own_units = &over_own_units;
    const auto site =
        match_->building_site(type, cell_x, cell_z, skip_unit, match_local_player_, options);
    const auto height = site ? *site : match_->footprint_height(type, cell_x, cell_z, facing);
    world[1] = static_cast<int32_t>(static_cast<uint32_t>(height) << 16);
    return PendingBuildSite{world, cell_x, cell_z, fx, fz, site.has_value(), over_own_units};
}

std::optional<Runtime::PendingBuildSite> Runtime::build_site_under(float x, float y) const {
    const auto target = build_cursor_point(x, y);
    if (!target)
        return std::nullopt;
    return pending_build_site(*target);
}

std::optional<oa::sim::ground_orders::Point> Runtime::build_cursor_point(float x, float y) const {
    return ground_point_under(x, y);
}

void Runtime::draw_build_ghost(
    oa::present::world_renderer::Surface& destination,
    const oa::present::world_renderer::BattlefieldViewport& viewport
) {
    rotate_hint_drawn_.clear();
    if (match_command_ != MatchCommand::build || pending_build_type_ == 0 || !selected_tnt_ ||
        pending_build_type_ >= spawn_types_.size())
        return;
    // The profile's line or ring build tool shows every building it lays.
    if ((build_tool_.drawing_line || build_tool_.ring) && !build_tool_.layout.slots.empty()) {
        for (const auto& slot : build_tool_.layout.slots)
            if (const auto placed =
                    pending_build_site({int32_t{slot.x} << 16, 0, int32_t{slot.z} << 16}))
                draw_build_site(destination, viewport, *placed);
        return;
    }
    auto site = build_site_under(match_pointer_x_, match_pointer_y_);
    // A click the profile's click snap would move shows where it goes, as
    // a site that may be built on.
    if (auto snapped = snapped_build_site(match_pointer_x_, match_pointer_y_)) {
        site = snapped;
        site->legal = true;
    }
    if (!site)
        return;
    draw_build_site(destination, viewport, *site);
    draw_build_facing(destination, viewport, *site);
}

void Runtime::draw_build_queue_drag(
    oa::present::world_renderer::Surface& destination,
    const oa::present::world_renderer::BattlefieldViewport& viewport
) {
    if (!build_queue_drag_ || !build_queue_drag_->moved)
        return;
    const auto ground = build_cursor_point(match_pointer_x_, match_pointer_y_);
    if (!ground)
        return;
    const auto site = build_site_for(build_queue_drag_->type, build_queue_drag_->facing, *ground);
    if (!site)
        return;
    draw_build_site(destination, viewport, *site);
}

void Runtime::draw_build_facing(
    oa::present::world_renderer::Surface& destination,
    const oa::present::world_renderer::BattlefieldViewport& viewport,
    const PendingBuildSite& site
) {
    // A building that may face more than one way shows its facing's letter
    // at its middle, and, until the player has turned one, how to turn it.
    rotate_hint_drawn_.clear();
    if (!ui_rules().build_preview.enabled)
        return;
    const auto facings = pending_build_facings();
    if (view_rules::next_build_facing(facings, view_rules::BuildFacing::south, 1) ==
        view_rules::BuildFacing::south)
        return;
    const auto middle = project_match_point(
        viewport,
        {static_cast<uint32_t>(site.world[0]),
         static_cast<uint32_t>(site.world[1]),
         static_cast<uint32_t>(site.world[2])}
    );
    ensure_ui_colors();
    renderer::Surface text{destination.width, destination.height, std::move(destination.rgb)};
    auto* const kept_target = overlay_target_;
    overlay_target_ = &text;
    const char letter[2] = {view_rules::facing_letter(pending_build_facing()), '\0'};
    draw_match_label(middle.x, middle.y, letter, ui_colors_[kUiColorText]);
    if (!view_settings_.rotate_key_discovered) {
        rotate_hint_drawn_ = rotate_hint_line();
        if (!rotate_hint_drawn_.empty())
            draw_match_label(
                middle.x, middle.y + kRotateHintRise, rotate_hint_drawn_, ui_colors_[kUiColorText]
            );
    }
    overlay_target_ = kept_target;
    destination.rgb = std::move(text.rgb);
}

void Runtime::draw_build_site(
    oa::present::world_renderer::Surface& destination,
    const oa::present::world_renderer::BattlefieldViewport& viewport,
    const PendingBuildSite& site_record
) {
    const auto* site = &site_record;
    const auto height = static_cast<uint32_t>(site->world[1]);
    // Cells to 16.16 map positions; a negative cell stays left of or above the map.
    const auto cell_position = [](int32_t cell) {
        return static_cast<uint32_t>(cell * OA_MAP_CELL_PIXELS) << 16;
    };
    const auto top_left = project_match_point(
        viewport, {cell_position(site->cell_x), height, cell_position(site->cell_z)}
    );
    const auto bottom_right = project_match_point(
        viewport,
        {cell_position(site->cell_x + site->footprint_x),
         height,
         cell_position(site->cell_z + site->footprint_z)}
    );
    ensure_ui_colors();
    // The profile's build tools show a site the local player's own units
    // must leave in a colour of its own.
    auto slot = site->legal ? kBuildSiteClearColor : kBuildSiteRefusedColor;
    if (site->legal && site->over_own_units && ui_rules().build_tools.enabled)
        slot = kBuildSiteOverOwnUnitsColor;
    const auto color = ui_color_rgb(slot);
    // The outlines lie side by side, each inset one pixel inside the last, so
    // together they are one band; the lines clip to the battlefield.
    const auto band = kBuildSiteOutlineCount * build_site_outline_width(viewport.scale);
    for (int inset = 0; inset < band; ++inset) {
        const auto left = top_left.x + inset;
        const auto top = top_left.y + inset;
        const auto right = bottom_right.x - inset;
        const auto bottom = bottom_right.y - inset;
        if (left > right || top > bottom)
            break;
        draw_match_line(destination, left, top, right, top, color);
        draw_match_line(destination, right, top, right, bottom, color);
        draw_match_line(destination, right, bottom, left, bottom, color);
        draw_match_line(destination, left, bottom, left, top, color);
    }
}

int Runtime::build_site_outline_width(float zoom) {
    // Also one pixel for a zoom that is no number.
    if (!(zoom >= 1.0F))
        return 1;
    const auto steps = static_cast<int>(std::lround(std::min(zoom, kMaxBattlefieldZoom)));
    return std::max(1, steps * kBuildSiteOutlinePixelsPerZoomStep);
}

oa::present::world_renderer::ScreenPoint Runtime::project_match_point(
    const oa::present::world_renderer::BattlefieldViewport& viewport,
    const std::array<uint32_t, 3>& position
) const {
    return project_world_point(viewport, position, HeightLift::nearest);
}

namespace {

/// Returns the whole battlefield frame as a draw's target: every row may
/// change, within the visible world rectangle.
///
/// @param destination the battlefield frame
/// @param x the visible world rectangle's left column
/// @param y its top row
/// @param width its columns
/// @param height its rows
/// @return the target
WorldTarget whole_frame(
    oa::present::world_renderer::Surface& destination, int x, int y, int width, int height
) {
    return {
        destination.rgb.data(),
        static_cast<int32_t>(destination.width),
        static_cast<int32_t>(destination.height),
        x,
        y,
        width,
        height,
        0,
        static_cast<int32_t>(destination.height)
    };
}

} // namespace

void Runtime::put_match_pixel(
    oa::present::world_renderer::Surface& destination,
    int x,
    int y,
    const std::array<uint8_t, 3>& color
) {
    put_world_pixel(
        whole_frame(
            destination,
            world_pixel_clip_.x,
            world_pixel_clip_.y,
            world_pixel_clip_.w,
            world_pixel_clip_.h
        ),
        x,
        y,
        color
    );
}

void Runtime::draw_match_line(
    oa::present::world_renderer::Surface& destination,
    int x0,
    int y0,
    int x1,
    int y1,
    const std::array<uint8_t, 3>& color
) {
    draw_world_line(
        whole_frame(
            destination,
            world_pixel_clip_.x,
            world_pixel_clip_.y,
            world_pixel_clip_.w,
            world_pixel_clip_.h
        ),
        x0,
        y0,
        x1,
        y1,
        color
    );
}

void Runtime::blit_gaf_on_world(
    oa::present::world_renderer::Surface& destination,
    const oa::formats::gaf::RenderedFrame& frame,
    int destination_x,
    int destination_y,
    const oa::PaletteBytes& palette,
    float scale
) {
    blit_world_frame(
        whole_frame(
            destination,
            world_pixel_clip_.x,
            world_pixel_clip_.y,
            world_pixel_clip_.w,
            world_pixel_clip_.h
        ),
        frame,
        destination_x,
        destination_y,
        palette,
        scale
    );
}

void Runtime::blit_gaf_hotspot(
    oa::present::world_renderer::Surface& destination,
    const oa::formats::gaf::RenderedFrame& frame,
    const oa::present::world_renderer::ScreenPoint& screen,
    const oa::PaletteBytes& palette,
    float scale
) {
    blit_world_hotspot(
        whole_frame(
            destination,
            world_pixel_clip_.x,
            world_pixel_clip_.y,
            world_pixel_clip_.w,
            world_pixel_clip_.h
        ),
        frame,
        screen,
        palette,
        scale
    );
}

std::array<uint8_t, 3> Runtime::ui_color_rgb(uint8_t index) const {
    return palette_rgb(ui_colors_[index]);
}

namespace {

// Weapon render types dispatched by the projectile pass.
enum class ProjectileRender : uint8_t {
    laser = 0,
    lens = 2,
    plasma = 4,
    flame = 5,
    lightning = 7,
};

// FX.GAF plasma sprites selected by the weapon colour for render type 4.
constexpr std::array<std::string_view, 5> kPlasmaSprites{
    "cannonshell", "plasmasm", "plasmamd", "ultrashell", "plasmasm"
};
constexpr uint8_t kNoPlasmaSprite = 0xff;
// Lightning is broken into segments of this many world units, each end
// jittered by up to +-5 units, drawn twice.
constexpr int64_t kLightningSegment = 5;
constexpr int32_t kLightningJitter = 5;
constexpr int kLightningPasses = 2;

std::array<uint32_t, 3> fixed_point(int64_t x, int64_t y, int64_t z) {
    return {static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z)};
}

} // namespace

void Runtime::plan_match_projectiles(
    WorldDrawList& draws,
    const oa::present::world_renderer::BattlefieldViewport& viewport,
    const oa::sim::effect_particles::ExplosionView& view
) {
    if (!match_)
        return;
    // Each line and sprite joins the frame's draws in the order the pass
    // draws them.
    const auto line = [&](int x0, int y0, int x1, int y1, const std::array<uint8_t, 3>& color) {
        draws.lines.push_back({x0, y0, x1, y1, color, 0});
        add_world_draw(draws, WorldDrawKind::line, draws.lines.size() - 1);
    };
    // A sprite stands over the map pixels of its point at any scale.
    const auto sprite = [&](const oa::formats::gaf::Frame& frame,
                            const std::array<uint32_t, 3>& position) {
        const auto* decoded = decoded_frame(draws, frame);
        if (decoded == nullptr)
            return;
        draws.sprites.push_back(
            {decoded, place_world_point(viewport, position, HeightLift::nearest)}
        );
        draws.sprites.back().world_position = oa::FixedVec3{
            std::bit_cast<int32_t>(position[0]),
            std::bit_cast<int32_t>(position[1]),
            std::bit_cast<int32_t>(position[2])
        };
        add_world_draw(draws, WorldDrawKind::sprite, draws.sprites.size() - 1);
    };
    ensure_ui_colors();
    const uint32_t now = match_->simulation().tick;
    uint32_t jitter_seed = now * 0x343fdU + 0x269ec3U;
    const auto jitter = [&jitter_seed]() {
        jitter_seed = jitter_seed * 0x343fdU + 0x269ec3U;
        const auto sample = static_cast<int32_t>((jitter_seed >> 16) & 0x7fffU);
        return sample * (2 * kLightningJitter + 1) / 0x8000 - kLightningJitter;
    };
    // Each shot draws where the frame shows it (presented_shot), which the
    // frame's draw worked out for the pool as it is.
    const auto shots = match_->projectiles();
    const auto& shown_shots = match_models().presentation.presented_shots;
    for (std::size_t index = 0; index < shots.size(); ++index) {
        const auto& shot = shots[index];
        const auto* weapon = match_->projectile_weapon(shot);
        if (weapon == nullptr || shot.burst_remaining != 0)
            continue;
        try {
            if (!match_->point_shown(
                    static_cast<uint8_t>(match_view_player()),
                    oa::sim::match_runtime::fixed_words(shot.position)
                ))
                continue;
        } catch (const std::exception&) {
            continue;
        }
        const ShotPose shown =
            shown_shots.size() == shots.size() ? shown_shots[index] : shot_pose(shot);
        const auto shot_position = oa::sim::match_runtime::fixed_words(shown.position);
        const auto shot_origin = oa::sim::match_runtime::fixed_words(shown.origin);
        switch (static_cast<ProjectileRender>(weapon->rendertype)) {
        case ProjectileRender::laser: {
            auto head = project_match_point(viewport, shot_position);
            auto tail = project_match_point(viewport, shot_origin);
            if (weapon->color2 != 0) {
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                if (std::abs(head.y - tail.y) < std::abs(head.x - tail.x)) {
                    if (tail.x < head.x)
                        std::swap(head, tail);
                    x0 = head.x, y0 = head.y - 1, x1 = tail.x, y1 = tail.y - 1;
                } else {
                    if (tail.y < head.y)
                        std::swap(head, tail);
                    x0 = head.x - 1, y0 = head.y, x1 = tail.x + 1, y1 = tail.y;
                }
                line(x0, y0, x1, y1, ui_color_rgb(weapon->color2));
            }
            line(head.x, head.y, tail.x, tail.y, ui_color_rgb(weapon->color));
            break;
        }
        case ProjectileRender::lens:
            // A lens whose shot's place at the tick is off the battlefield
            // is not drawn, and the pass goes on to the next projectile.
            // One on it is drawn where the frame shows the shot, raised by
            // half its height rounded down as the test raises it, and
            // clipped to the view.
            if (!projectile_lens_on_battlefield(
                    view, oa::sim::match_runtime::fixed_words(shot.position)
                ))
                break;
            draws.lenses.push_back(project_world_point(viewport, shot_position, HeightLift::down));
            add_world_draw(draws, WorldDrawKind::lens, draws.lenses.size() - 1);
            break;
        case ProjectileRender::plasma: {
            if (weapon->color == kNoPlasmaSprite || weapon->color >= kPlasmaSprites.size())
                break;
            const auto* sequence = gaf_sequence(match_fx_, kPlasmaSprites[weapon->color]);
            if (sequence == nullptr || sequence->frames.empty())
                break;
            const auto frame = (now - shot.burst_tick) % sequence->frames.size();
            const auto* definition = oa::world_weapon_def(&match_->state(), shot.def);
            if ((full_presentation() || live_renderer_active()) && definition &&
                renderer_weapon_style::is_emg(*definition)) {
                // A short fading train follows the actual shot velocity, independent of frame rate.
                constexpr int trail_samples = 3;
                constexpr float trail_ticks = 0.45F;
                for (int sample = trail_samples - 1; sample >= 0; --sample) {
                    const float age = trail_ticks * sample / (trail_samples - 1);
                    auto point = shown.position;
                    point.x -= static_cast<int32_t>(shot.velocity.x * age);
                    point.y -= static_cast<int32_t>(shot.velocity.y * age);
                    point.z -= static_cast<int32_t>(shot.velocity.z * age);
                    const auto before = draws.sprites.size();
                    sprite(sequence->frames[frame], oa::sim::match_runtime::fixed_words(point));
                    if (draws.sprites.size() != before) {
                        auto& drawn = draws.sprites.back();
                        drawn.effect_scale = sample == 0 ? 0.65F : 0.45F;
                        drawn.effect_opacity = 1.0F - sample * 0.3F;
                        drawn.soft_effect = true;
                    }
                }
            } else {
                sprite(sequence->frames[frame], shot_position);
            }
            break;
        }
        case ProjectileRender::flame: {
            const auto* sequence = gaf_sequence(match_fx_, "flamestream");
            if (sequence == nullptr || sequence->frames.empty() || weapon->weapontimer_ticks == 0)
                break;
            // The flame grows through the sequence as its lifetime runs out.
            const auto count = static_cast<int64_t>(sequence->frames.size());
            const auto remaining = static_cast<int64_t>(shot.lifetime_tick) - now;
            const auto frame = count - remaining * count / weapon->weapontimer_ticks;
            if (frame < 0 || frame >= count)
                break;
            sprite(sequence->frames[static_cast<std::size_t>(frame)], shot_position);
            break;
        }
        case ProjectileRender::lightning: {
            std::array<int64_t, 3> delta{};
            for (std::size_t axis = 0; axis < 3; ++axis)
                delta[axis] = static_cast<int64_t>(static_cast<int32_t>(shot_position[axis])) -
                              static_cast<int32_t>(shot_origin[axis]);
            const auto length = std::sqrt(
                static_cast<double>(delta[0]) * static_cast<double>(delta[0]) +
                static_cast<double>(delta[1]) * static_cast<double>(delta[1]) +
                static_cast<double>(delta[2]) * static_cast<double>(delta[2])
            );
            const auto segments = static_cast<int64_t>(length / 65536.0) / kLightningSegment;
            if (segments <= 0)
                break;
            const auto color = ui_color_rgb(weapon->color);
            for (int pass = 0; pass < kLightningPasses; ++pass) {
                std::array<int64_t, 3> previous{
                    static_cast<int32_t>(shot_origin[0]),
                    static_cast<int32_t>(shot_origin[1]),
                    static_cast<int32_t>(shot_origin[2])
                };
                for (int64_t step = 1; step <= segments; ++step) {
                    std::array<int64_t, 3> point{};
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        point[axis] = static_cast<int32_t>(shot_origin[axis]) +
                                      delta[axis] * step / segments +
                                      static_cast<int64_t>(jitter()) * 65536;
                    const auto from = project_match_point(
                        viewport, fixed_point(previous[0], previous[1], previous[2])
                    );
                    const auto to =
                        project_match_point(viewport, fixed_point(point[0], point[1], point[2]));
                    line(from.x, from.y, to.x, to.y, color);
                    previous = point;
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

} // namespace oa::app
