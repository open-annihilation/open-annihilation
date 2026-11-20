// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/galaxy.hpp"

#include "oa/boneyards/screens/contents.hpp"
#include "oa/boneyards/screens/rand15.hpp"
#include "text.hpp"

#include <cstdio>
#include <cstring>
#include <type_traits>

namespace oa::boneyards::screens {
namespace {

constexpr int32_t kDefaultScrollSteps[kGalaxyScaleCount] = {0x14, 0xe, 10, 3};

constexpr int32_t kVariantGroupCount = 3;
constexpr int32_t kPlanetRadii[kVariantGroupCount] = {0x14, 0x10, 0xc};

// A star cell is this share of the map image width, truncated.
constexpr double kStarCellShare = 0.05;
// Per layer: the share of the scroll range its window travels (the farthest
// layer stays still), its dim and its seed's offset from the sector id.
constexpr float kStarScrollShares[kGalaxyStarLayers] = {0.4f, 0.2f, 0.0f};
constexpr int32_t kStarLayerDims[kGalaxyStarLayers] = {0, 2, 4};
constexpr int32_t kStarKindPercents[kStarKindChances] = {80, 10, 5};
constexpr int32_t kPercentRange = 100;

// Choices of a star's variant and dim (the dim is twice the choice).
constexpr int32_t kPairVariants = 4;
constexpr int32_t kSquareVariants = 2;
constexpr int32_t kPointDims = 6;
constexpr int32_t kShapeDims = 4;
constexpr int32_t kDimStep = 2;
// Pair variants below this lie across, the others down.
constexpr int32_t kFirstUprightPair = 2;
constexpr int32_t kCrossSpan = 2;

// Recon pulses the wind waits between shifts are drawn below this.
constexpr int32_t kWindCountdownRange = 0x51;

constexpr const char* kUnknownValue = "?";
constexpr const char* kNotApplicable = "n/a";

// View pixels for a map-unit distance at the current zoom level.
int32_t to_view(const GalaxyView* v, int32_t map_units) {
    return map_units * (v->scale + 1) / kMapUnitsPerPixelAtLevel0;
}

void set_view_extent(GalaxyView* v) {
    const int32_t divisor = v->scale + 1;
    v->view_width = kGalaxyMapWidth / divisor;
    v->view_height = kGalaxyMapHeight / divisor;
}

char upper_side(char side) {
    return (side >= 'a' && side <= 'z') ? static_cast<char>(side - 'a' + 'A') : side;
}

void project_all(GalaxyView* v) {
    for (std::size_t i = 0; i < v->connector_count; ++i)
        galaxy_project_connector(v, &v->connectors[i]);
    for (std::size_t i = 0; i < v->planet_count; ++i)
        galaxy_place_hotspot(v, &v->planets[i]);
}

/// Joins two planets: their anchors become the endpoints, with nothing projected or coloured yet.
///
/// @param[out] c connector, cleared first
/// @param v galaxy view holding the planets
/// @param a first planet index
/// @param b second planet index
void link_connector(GalaxyConnector* c, const GalaxyView* v, int32_t a, int32_t b) {
    std::memset(c, 0, sizeof *c);
    c->a = a;
    c->b = b;
    c->ax = v->planets[a].x;
    c->ay = v->planets[a].y;
    c->bx = v->planets[b].x;
    c->by = v->planets[b].y;
}

void scale_star_layers(GalaxyView* v) {
    for (GalaxyStarLayer& layer : v->star_layers)
        galaxy_scale_star_layer(&layer, v->scroll_x, v->scroll_y, v->view_width, v->view_height);
}

/// Builds the three star-field layers of a freshly opened map, seeded from the sector id.
///
/// Each layer steps a generator state of its own, so building the layers leaves every other
/// random draw as it was; in 3.1c the layers share one sequence with those draws, which then
/// continue from where the layers left it.
///
/// @param[in,out] v the galaxy view whose layers are built and scaled
/// @return false when a layer could not be built
bool build_star_layers(GalaxyView* v) {
    const auto cell_size = static_cast<int32_t>(static_cast<double>(v->map_width) * kStarCellShare);
    for (std::size_t i = 0; i < kGalaxyStarLayers; ++i) {
        const auto seed = static_cast<uint32_t>(v->sector_id + static_cast<int32_t>(i));
        if (!galaxy_build_star_layer(
                &v->star_layers[i],
                v->map_width,
                v->map_height,
                seed,
                kStarScrollShares[i],
                cell_size,
                kStarLayerDims[i],
                kStarKindPercents
            ))
            return false;
    }
    scale_star_layers(v);
    return true;
}

// Marks the map data usable and refreshes the territory summary (the tail
// shared by the allegiance and activity feeds).
void feed_applied(GalaxyView* v) {
    v->has_data = true;
    v->loading = false;
    if (v->territory_attached)
        territory_panel_update(&v->territory, v);
}

} // namespace

void galaxy_init(GalaxyView* v) {
    // The view is trivially copyable and too large for a temporary.
    static_assert(std::is_trivially_copyable_v<GalaxyView>);
    std::memset(static_cast<void*>(v), 0, sizeof *v);
    v->loading = true;
    v->locked = true;
    v->selected = kNoPlanet;
    v->hovered = kNoPlanet;
    v->territory.planet = kNoPlanet;
    for (std::size_t i = 0; i < kGalaxyScaleCount; ++i)
        v->scroll_step[i] = kDefaultScrollSteps[i];
    set_view_extent(v);
}

void galaxy_clear(GalaxyView* v) {
    v->has_data = false;
    v->show_map_while_loading = false;
    v->loading = true;
    v->animation_steps = 0;
    v->scroll_x = 0;
    v->scroll_y = 0;
    v->selected = kNoPlanet;
    v->hovered = kNoPlanet;
    v->planet_count = 0;
    v->connector_count = 0;
    for (GalaxyStarLayer& layer : v->star_layers)
        layer.star_count = 0;
}

void galaxy_step_animation(GalaxyView* v, int32_t static_frames) {
    // A zero frame count is skipped; the war-map graphics and planet sprites
    // always have frames.
    if (v->loading && static_frames > 0)
        v->static_frame = (v->static_frame + 1) % static_frames;
    if (!v->has_data)
        return;
    for (std::size_t i = 0; i < v->planet_count; ++i) {
        GalaxyPlanet& p = v->planets[i];
        if (p.frame_count > 0 &&
            (p.frame_interval == 0 || v->animation_steps % p.frame_interval == 0))
            p.frame = (p.frame + 1) % p.frame_count;
    }
    ++v->animation_steps;
}

MapSource map_source_from_key(const char* key) {
    struct Entry {
        const char* key;
        MapSource source;
    };

    static constexpr Entry kEntries[] = {
        {"TA", MapSource::ta_disk},
        {"BT", MapSource::battle_tactics},
        {"CC", MapSource::core_contingency},
        {"WTA", MapSource::web_ta},
        {"WCC", MapSource::web_core_contingency},
        {"WBT", MapSource::web_battle_tactics},
    };
    for (const Entry& e : kEntries)
        if (equals_nocase(key, e.key))
            return e.source;
    return MapSource::ta_disk;
}

void galaxy_build_planet(GalaxyPlanet* p, const PlanetRecord* r) {
    // The planet is trivially copyable; it is cleared in place.
    static_assert(std::is_trivially_copyable_v<GalaxyPlanet>);
    std::memset(static_cast<void*>(p), 0, sizeof *p);
    p->id = r->id;
    p->size_class = r->variant_group;
    p->frame_interval = r->frame_interval;
    p->x = r->x;
    p->y = r->y;
    p->side = r->side;
    p->initial_side = r->side;
    p->capitol = r->capitol;
    p->player_limit = r->settings[0];
    p->commander_death = r->settings[1];
    for (std::size_t i = 2; i < kPlanetSettingCount; ++i)
        p->extra_settings[i - 2] = r->settings[i];
    p->left = p->right = p->view_x = r->x;
    p->top = p->bottom = p->view_y = r->y;
    copy_bounded(p->name, sizeof p->name, r->name);
    copy_bounded(p->ta_map, sizeof p->ta_map, r->ta_map);
    p->map_source = map_source_from_key(r->ta_mapsource);
    copy_bounded(p->map_type, sizeof p->map_type, r->ta_maptype);
}

bool galaxy_set_planet_sprite(
    GalaxyPlanet* p, const PlanetRecord* r, const SpriteFrame* frames, std::size_t frame_count
) {
    copy_bounded(p->description, sizeof p->description, r->description);
    if (r->variant_group < 0 || r->variant_group >= kVariantGroupCount)
        return false;
    p->frame_count = static_cast<int32_t>(frame_count);
    p->radius = kPlanetRadii[r->variant_group];
    for (std::size_t i = 0; i < frame_count; ++i) {
        const SpriteFrame& f = frames[i];
        const int32_t left = p->x - f.x_offset;
        const int32_t top = p->y - f.y_offset;
        const int32_t right = left + f.width - 1;
        const int32_t bottom = top + f.height - 1;
        if (left < p->left)
            p->left = left;
        if (top < p->top)
            p->top = top;
        if (right > p->right)
            p->right = right;
        if (bottom > p->bottom)
            p->bottom = bottom;
    }
    std::snprintf(
        p->pan_gaf, sizeof p->pan_gaf, "anims\\%s", r->pan_gaf != nullptr ? r->pan_gaf : ""
    );
    copy_bounded(p->pan_seq, sizeof p->pan_seq, r->pan_seq);
    return true;
}

bool galaxy_load(
    GalaxyView* v,
    int32_t map_width,
    int32_t map_height,
    const PlanetRecord* planets,
    std::size_t planet_count,
    const ConnectorRecord* connectors,
    std::size_t connector_count,
    const PlanetSprites* sprites
) {
    galaxy_clear(v);
    v->map_width = map_width;
    v->map_height = map_height;
    if (map_width < kGalaxyMapWidth || map_height < kGalaxyMapHeight)
        return false;
    v->max_scroll_x = map_width - v->view_width;
    v->max_scroll_y = map_height - v->view_height;
    if (!build_star_layers(v))
        return false;
    if (planet_count > kMaxGalaxyPlanets || connector_count > kMaxGalaxyConnectors)
        return false;
    const SpriteFrame* frames = sprites != nullptr ? sprites->frames : nullptr;
    for (std::size_t i = 0; i < planet_count; ++i) {
        GalaxyPlanet planet;
        galaxy_build_planet(&planet, &planets[i]);
        const std::size_t frame_count = sprites != nullptr ? sprites->frame_counts[i] : 0;
        if (!galaxy_set_planet_sprite(&planet, &planets[i], frames, frame_count))
            return false;
        frames += frame_count;
        galaxy_place_hotspot(v, &planet);
        std::size_t at = 0;
        while (at < v->planet_count && v->planets[at].id < planet.id)
            ++at;
        if (at < v->planet_count && v->planets[at].id == planet.id)
            return false;
        std::memmove(
            &v->planets[at + 1], &v->planets[at], sizeof(GalaxyPlanet) * (v->planet_count - at)
        );
        v->planets[at] = planet;
        ++v->planet_count;
    }
    for (std::size_t i = 0; i < connector_count; ++i) {
        const int32_t a = galaxy_find_planet(v, connectors[i].planet1);
        const int32_t b = galaxy_find_planet(v, connectors[i].planet2);
        if (a == kNoPlanet || b == kNoPlanet)
            return false;
        GalaxyConnector* c = &v->connectors[v->connector_count++];
        link_connector(c, v, a, b);
        galaxy_project_connector(v, c);
        galaxy_color_connector(v, c);
    }
    return true;
}

int32_t galaxy_find_planet(const GalaxyView* v, int32_t id) {
    for (std::size_t i = 0; i < v->planet_count; ++i)
        if (v->planets[i].id == id)
            return static_cast<int32_t>(i);
    return kNoPlanet;
}

int32_t galaxy_player_capitol(const GalaxyView* v, char allegiance) {
    if (v == nullptr)
        return kNoPlanet;
    const char side = upper_side(allegiance);
    if (side != 'A' && side != 'C')
        return kNoPlanet;
    for (std::size_t i = 0; i < v->planet_count; ++i) {
        const GalaxyPlanet& planet = v->planets[i];
        if (planet.capitol && upper_side(planet.initial_side) == side)
            return static_cast<int32_t>(i);
    }
    return kNoPlanet;
}

void galaxy_place_hotspot(GalaxyView* v, GalaxyPlanet* p) {
    const bool visible = p->left < v->scroll_x + v->view_width &&
                         p->top < v->scroll_y + v->view_height && v->scroll_x <= p->right &&
                         v->scroll_y <= p->bottom;
    p->view_x = to_view(v, p->x - v->scroll_x);
    p->view_y = to_view(v, p->y - v->scroll_y);
    p->view_left = to_view(v, p->left - v->scroll_x);
    p->view_top = to_view(v, p->top - v->scroll_y);
    p->view_width = to_view(v, p->right - p->left + 1);
    p->view_height = to_view(v, p->bottom - p->top + 1);
    if (!p->visible) {
        if (visible)
            p->hotspot = true;
    } else if (!visible) {
        p->hotspot = false;
    }
    p->visible = visible;
}

void galaxy_project_connector(const GalaxyView* v, GalaxyConnector* c) {
    const int32_t right = v->scroll_x - 1 + v->view_width;
    const int32_t bottom = v->scroll_y - 1 + v->view_height;
    const auto outside = [&](int32_t x, int32_t y) {
        return ((x < v->scroll_x) << 3) | ((right < x) << 2) | ((y < v->scroll_y) << 1) |
               (bottom < y);
    };
    c->visible = false;
    if ((outside(c->ax, c->ay) & outside(c->bx, c->by)) != 0)
        return;
    c->view_ax = to_view(v, c->ax - v->scroll_x);
    c->view_ay = to_view(v, c->ay - v->scroll_y);
    c->view_bx = to_view(v, c->bx - v->scroll_x);
    c->view_by = to_view(v, c->by - v->scroll_y);
    c->visible = true;
}

void galaxy_color_connector(const GalaxyView* v, GalaxyConnector* c) {
    const char a = upper_side(v->planets[c->a].side);
    const char b = upper_side(v->planets[c->b].side);
    switch (a) {
    case 'A':
        if (b == 'C')
            c->color = connector_color::mixed;
        else if (b == 'A' || b == 'I')
            c->color = connector_color::arm;
        break;
    case 'C':
        if (b == 'A')
            c->color = connector_color::mixed;
        else if (b == 'C' || b == 'I')
            c->color = connector_color::core;
        break;
    case 'I':
        if (b == 'A')
            c->color = connector_color::arm;
        else if (b == 'C')
            c->color = connector_color::core;
        else if (b == 'I')
            c->color = connector_color::mixed;
        break;
    default:
        break;
    }
}

bool galaxy_build_star_layer(
    GalaxyStarLayer* layer,
    int32_t width,
    int32_t height,
    uint32_t seed,
    float scroll_share,
    int32_t cell_size,
    int32_t dim,
    const int32_t kind_percents[kStarKindChances]
) {
    layer->width = width;
    layer->height = height;
    layer->scroll_share = scroll_share;
    layer->dim = dim;
    layer->star_count = 0;
    if (cell_size < 2 || width <= 0 || height <= 0)
        return false;
    const int32_t columns = width / cell_size + (width % cell_size != 0 ? 1 : 0);
    const int32_t rows = height / cell_size + (height % cell_size != 0 ? 1 : 0);
    if (static_cast<int64_t>(columns) * rows > static_cast<int64_t>(kMaxGalaxyStars))
        return false;
    const int32_t cells = columns * rows;
    std::memset(layer->stars, 0, sizeof(GalaxyStar) * static_cast<std::size_t>(cells));
    uint32_t holdrand = seed;
    const int32_t spread = cell_size - 1;
    for (int32_t row = 0; row < rows; ++row) {
        for (int32_t column = 0; column < columns; ++column) {
            const int32_t x = rand15_step(&holdrand) % spread + column * cell_size;
            const int32_t y = rand15_step(&holdrand) % spread + row * cell_size;
            if (x >= width || y >= height)
                continue;
            const int32_t roll = rand15_step(&holdrand) % kPercentRange;
            StarKind kind = StarKind::cross;
            if (roll < kind_percents[0])
                kind = StarKind::point;
            else if (roll < kind_percents[0] + kind_percents[1])
                kind = StarKind::pair;
            else if (roll < kind_percents[0] + kind_percents[1] + kind_percents[2])
                kind = StarKind::square;
            galaxy_place_star(&layer->stars[row * columns + column], x, y, kind, &holdrand);
        }
    }
    layer->star_count = cells;
    return true;
}

void galaxy_place_star(GalaxyStar* star, int32_t x, int32_t y, StarKind kind, uint32_t* holdrand) {
    star->x = x;
    star->left = x;
    star->right = x;
    star->y = y;
    star->top = y;
    star->bottom = y;
    star->kind = kind;
    switch (kind) {
    case StarKind::point:
        star->variant = 0;
        star->dim = rand15_step(holdrand) % kPointDims * kDimStep;
        break;
    case StarKind::pair:
        star->variant = rand15_step(holdrand) % kPairVariants;
        star->dim = rand15_step(holdrand) % kShapeDims * kDimStep;
        if (star->variant < kFirstUprightPair)
            ++star->right;
        else
            ++star->bottom;
        break;
    case StarKind::square:
        star->variant = rand15_step(holdrand) % kSquareVariants;
        star->dim = rand15_step(holdrand) % kShapeDims * kDimStep;
        ++star->right;
        ++star->bottom;
        break;
    case StarKind::cross:
        star->variant = 0;
        star->dim = rand15_step(holdrand) % kShapeDims * kDimStep;
        star->right += kCrossSpan;
        star->bottom += kCrossSpan;
        break;
    default:
        star->variant = 0;
        star->kind = StarKind::point;
        star->dim = rand15_step(holdrand) % kPointDims;
        break;
    }
}

void galaxy_project_star(
    GalaxyStar* star, int32_t left, int32_t top, int32_t width, int32_t height
) {
    star->visible = star->left < left + width && star->top < top + height && left <= star->right &&
                    top <= star->bottom;
    star->view_x = (star->x - left) * kGalaxyViewerWidth / width;
    star->view_y = (star->y - top) * kGalaxyViewerHeight / height;
}

void galaxy_scale_star_layer(
    GalaxyStarLayer* layer,
    int32_t scroll_x,
    int32_t scroll_y,
    int32_t view_width,
    int32_t view_height
) {
    const int32_t range_x = layer->width - view_width + 1;
    const int32_t range_y = layer->height - view_height + 1;
    const auto travel_x = static_cast<int32_t>(static_cast<double>(range_x) * layer->scroll_share);
    const auto travel_y = static_cast<int32_t>(static_cast<double>(range_y) * layer->scroll_share);
    int32_t window_width = layer->width - travel_x + 1;
    int32_t left = 0;
    if (window_width > layer->width)
        window_width = layer->width;
    else
        left = travel_x * scroll_x / range_x;
    int32_t window_height = layer->height - travel_y + 1;
    int32_t top = 0;
    if (window_height > layer->height)
        window_height = layer->height;
    else
        top = travel_y * scroll_y / range_y;
    for (int32_t i = 0; i < layer->star_count; ++i)
        galaxy_project_star(&layer->stars[i], left, top, window_width, window_height);
}

void galaxy_scroll(GalaxyView* v, int32_t dx, int32_t dy, bool absolute) {
    if (!v->has_data || v->loading)
        return;
    int32_t x;
    int32_t y;
    if (absolute) {
        x = dx + v->scroll_x;
        y = dy + v->scroll_y;
    } else {
        const int32_t step = v->scroll_step[v->scale];
        x = step * dx + v->scroll_x;
        y = step * dy + v->scroll_y;
    }
    if (x < 0)
        x = 0;
    else if (x > v->max_scroll_x)
        x = v->max_scroll_x;
    if (y < 0)
        y = 0;
    else if (y > v->max_scroll_y)
        y = v->max_scroll_y;
    if (x == v->scroll_x && y == v->scroll_y && !absolute)
        return;
    v->scroll_x = x;
    v->scroll_y = y;
    project_all(v);
    scale_star_layers(v);
}

void galaxy_set_scale(GalaxyView* v, int32_t scale) {
    if (!v->has_data || v->loading || scale == v->scale || scale < 0 || scale >= kGalaxyScaleCount)
        return;
    v->scale = scale;
    set_view_extent(v);
    v->max_scroll_x = v->map_width - v->view_width;
    v->max_scroll_y = v->map_height - v->view_height;
    int32_t dx = 0;
    int32_t dy = 0;
    if (v->selected != kNoPlanet) {
        const GalaxyPlanet& s = v->planets[v->selected];
        dy = (s.y - s.view_y * kMapUnitsPerPixelAtLevel0 / (scale + 1)) - v->scroll_y;
        dx = (s.x - s.view_x * kMapUnitsPerPixelAtLevel0 / (scale + 1)) - v->scroll_x;
    }
    galaxy_scroll(v, dx, dy, true);
}

bool galaxy_apply_census(GalaxyView* v, const CensusReport* r, uint32_t current_area) {
    const int32_t index = galaxy_find_planet(v, static_cast<int32_t>(r->area_id));
    if (index == kNoPlanet)
        return false;
    GalaxyPlanet& p = v->planets[index];
    p.population = r->population;
    p.activity = r->activity;
    p.in_progress = r->in_progress;
    p.forming = r->forming;
    p.arm_wins = r->arm_wins;
    p.core_wins = r->core_wins;
    if (r->area_id == current_area && v->territory_attached)
        territory_panel_update(&v->territory, v);
    return true;
}

void galaxy_apply_allegiances(GalaxyView* v, const char* allegiances) {
    for (std::size_t i = 0; i < v->planet_count && allegiances != nullptr && allegiances[i] != '\0';
         ++i)
        v->planets[i].side = allegiances[i];
    for (std::size_t i = 0; i < v->connector_count; ++i)
        galaxy_color_connector(v, &v->connectors[i]);
    feed_applied(v);
}

void galaxy_apply_activity(GalaxyView* v, const char* digits) {
    for (std::size_t i = 0; i < v->planet_count && digits != nullptr && digits[i] != '\0'; ++i)
        v->planets[i].activity = digits[i] - '0';
    feed_applied(v);
}

void territory_panel_update(TerritoryPanel* t, const GalaxyView* v) {
    if (t->planet == kNoPlanet || t->planet >= static_cast<int32_t>(v->planet_count)) {
        if (!t->shown)
            return;
        t->shown = false;
        t->contested_shown = false;
        return;
    }
    const GalaxyPlanet& p = v->planets[t->planet];
    copy_bounded(t->name, sizeof t->name, p.name);
    const char* owner = planet_affiliation_label(p.side);
    if (owner != nullptr)
        t->owner = owner;
    const bool contested = upper_side(p.side) == 'I';
    count_label_set(&t->in_progress, static_cast<uint32_t>(p.in_progress));
    count_label_set(&t->forming, static_cast<uint32_t>(p.forming));
    t->shown = true;
    if (contested) {
        count_label_set(&t->arm_wins, static_cast<uint32_t>(p.arm_wins));
        count_label_set(&t->core_wins, static_cast<uint32_t>(p.core_wins));
        t->contested_shown = true;
        return;
    }
    t->contested_shown = false;
}

bool galaxy_select_planet(GalaxyView* v, int32_t planet, uint32_t* area_id) {
    if (v->locked || planet < 0 || planet >= static_cast<int32_t>(v->planet_count))
        return false;
    if (area_id != nullptr)
        *area_id = static_cast<uint32_t>(v->planets[planet].id);
    v->selected = planet;
    v->territory.planet = planet;
    territory_panel_update(&v->territory, v);
    return true;
}

bool galaxy_hover_planet(GalaxyView* v, int32_t planet) {
    if (v->locked)
        return false;
    v->hovered = planet;
    return true;
}

bool galaxy_shows_war_map(const GalaxyView* v) {
    return v->has_data && (!v->loading || v->show_map_while_loading);
}

bool galaxy_load_environment(GalaxyPlanet* p, const MapTdfReader* reader) {
    PlanetEnvironment& e = p->environment;
    if (e.loaded)
        return true;
    if (reader == nullptr || reader->open == nullptr)
        return false;
    if (reader->note_source != nullptr)
        reader->note_source(reader->user, p->map_source);
    if (!reader->open(reader->user, p->ta_map))
        return false;
    e.loaded = true;
    e.gravity = reader->get_int(reader->user, "gravity", 0);
    e.min_wind = reader->get_int(reader->user, "minwindspeed", 0);
    e.max_wind = reader->get_int(reader->user, "maxwindspeed", 0);
    e.tidal_strength = reader->get_int(reader->user, "tidalstrength", 0);
    e.lavaworld = reader->get_int(reader->user, "lavaworld", 0) != 0;
    return true;
}

void planet_info_fill(
    PlanetInfo* info,
    GalaxyPlanet* p,
    const MapTdfReader* reader,
    RandomSource random,
    void* random_user
) {
    namespace row = planet_info_row;
    std::memset(info, 0, sizeof *info);
    info->labels[row::planet] = "Planet:";
    info->labels[row::gw_map] = "GW Map:";
    info->labels[row::type] = "Type:";
    info->labels[row::gravity] = "Gravity:";
    info->labels[row::wind_speed] = "Wind Speed:";
    info->labels[row::tide_strength] = "Tide Strength:";
    info->labels[row::affiliation] = "Affiliation:";
    info->labels[row::population] = "Population:";
    copy_bounded(info->values[row::planet], kPlanetInfoValueCapacity, p->name);
    copy_bounded(info->values[row::gw_map], kPlanetInfoValueCapacity, p->ta_map);
    copy_bounded(info->values[row::type], kPlanetInfoValueCapacity, p->map_type);
    info->map_installed = galaxy_load_environment(p, reader);
    if (!info->map_installed) {
        for (std::size_t i = row::gravity; i <= row::tide_strength; ++i)
            copy_bounded(info->values[i], kPlanetInfoValueCapacity, kUnknownValue);
    } else {
        const PlanetEnvironment& e = p->environment;
        format_signed_label(
            info->values[row::gravity], kPlanetInfoValueCapacity, static_cast<uint32_t>(e.gravity)
        );
        if (random != nullptr)
            info->wind_countdown = random(random_user) % kWindCountdownRange;
        const int32_t range = e.max_wind - e.min_wind;
        info->wind_speed = e.min_wind;
        if (range != 0 && random != nullptr)
            info->wind_speed = random(random_user) % range + e.min_wind;
        format_signed_label(
            info->values[row::wind_speed],
            kPlanetInfoValueCapacity,
            static_cast<uint32_t>(info->wind_speed)
        );
        if (e.lavaworld)
            copy_bounded(
                info->values[row::tide_strength], kPlanetInfoValueCapacity, kNotApplicable
            );
        else
            format_signed_label(
                info->values[row::tide_strength],
                kPlanetInfoValueCapacity,
                static_cast<uint32_t>(e.tidal_strength)
            );
    }
    const char* owner = planet_affiliation_label(p->side);
    copy_bounded(
        info->values[row::affiliation], kPlanetInfoValueCapacity, owner != nullptr ? owner : ""
    );
    format_signed_label(
        info->values[row::population],
        kPlanetInfoValueCapacity,
        static_cast<uint32_t>(p->population)
    );
    if (!info->map_installed)
        info->notice_count =
            gw_requirement_lines(static_cast<int32_t>(p->map_source), info->notice);
    info->description = p->description;
}

void recon_begin(
    ReconState* state,
    int32_t planet,
    const GalaxyPlanet* record,
    const PlanetInfo* info,
    const int32_t* pan_frame_widths,
    std::size_t pan_frame_count
) {
    *state = ReconState{};
    state->planet = planet;
    state->map_installed = info->map_installed;
    if (state->map_installed) {
        const PlanetEnvironment& e = record->environment;
        state->wind_countdown = info->wind_countdown;
        state->wind_min = e.min_wind;
        state->wind_max = e.max_wind;
        state->wind_range = e.max_wind - e.min_wind;
        state->wind = info->wind_speed;
    }
    if (pan_frame_widths == nullptr || pan_frame_count == 0)
        return;
    if (pan_frame_count > kMaxReconPanFrames)
        pan_frame_count = kMaxReconPanFrames;
    for (std::size_t i = 0; i < pan_frame_count; ++i) {
        state->pan_offsets[i] = state->pan_total;
        state->pan_total += pan_frame_widths[i];
    }
    state->pan_frame_count = static_cast<int32_t>(pan_frame_count);
    state->has_pan = state->pan_total > 0;
}

void recon_step(ReconState* s, RandomSource random, void* random_user, int32_t static_frames) {
    if (!s->map_installed || !s->has_pan) {
        if (static_frames > 0)
            s->static_frame = (s->static_frame + 1) % static_frames;
        return;
    }
    if (s->wind_range != 0 && --s->wind_countdown < 1 && random != nullptr) {
        s->wind += random(random_user) % (2 * kReconWindJitter + 1) - kReconWindJitter;
        if (s->wind < s->wind_min)
            s->wind = s->wind_min;
        if (s->wind > s->wind_max)
            s->wind = s->wind_max;
        s->wind_dirty = true;
        s->wind_countdown = random(random_user) % kWindCountdownRange;
    }
    s->pan_scroll = (s->pan_scroll + 1) % s->pan_total;
    s->pan_end = (s->pan_scroll + kReconPanWidth) % s->pan_total;
}

} // namespace oa::boneyards::screens
