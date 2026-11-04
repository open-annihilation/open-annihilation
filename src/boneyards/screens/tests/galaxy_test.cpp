// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/galaxy.hpp"
#include "oa/boneyards/screens/rand15.hpp"
#include "oa/boneyards/screens/navigator.hpp"

#include "check.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace oa::boneyards::screens;

namespace {

PlanetRecord
planet(int32_t id, const char* name, int32_t x, int32_t y, char side, const char* source) {
    PlanetRecord r{};
    r.id = id;
    r.name = name;
    r.x = x;
    r.y = y;
    r.sequence = "Planet";
    r.side = side;
    r.description = "Lore";
    r.ta_map = "Coast to Coast";
    r.ta_mapsource = source;
    r.ta_maptype = "Green";
    r.pan_gaf = "pan.gaf";
    r.pan_seq = "seq";
    r.settings[0] = 4;
    r.settings[1] = 2;
    return r;
}

// 16 x 16 sprites anchored at their centre.
const SpriteFrame kFrames[3] = {{16, 16, 8, 8}, {16, 16, 8, 8}, {16, 16, 8, 8}};
const std::size_t kFrameCounts[3] = {1, 1, 1};
const PlanetSprites kSprites{kFrames, kFrameCounts};

struct Tdf {
    bool installed = true;
    int32_t gravity = 112, min_wind = 10, max_wind = 40, tidal = 20, lavaworld = 0;
    std::string opened;
};

bool tdf_open(void* user, const char* map) {
    auto* t = static_cast<Tdf*>(user);
    t->opened = map;
    return t->installed;
}

int32_t tdf_int(void* user, const char* key, int32_t fallback) {
    auto* t = static_cast<Tdf*>(user);
    if (!std::strcmp(key, "gravity"))
        return t->gravity;
    if (!std::strcmp(key, "minwindspeed"))
        return t->min_wind;
    if (!std::strcmp(key, "maxwindspeed"))
        return t->max_wind;
    if (!std::strcmp(key, "tidalstrength"))
        return t->tidal;
    if (!std::strcmp(key, "lavaworld"))
        return t->lavaworld;
    return fallback;
}

int32_t fixed_random(void*) {
    return 7;
}

GalaxyView g_view;

bool load_three(GalaxyView* v, int32_t width = 2400, int32_t height = 1600) {
    const PlanetRecord planets[3] = {
        planet(3, "Barathrum", 1000, 900, 'I', "WCC"),
        planet(1, "Core Prime", 10, 100, 'C', "CC"),
        planet(2, "Empyrrean", 100, 100, 'A', "ta"),
    };
    const ConnectorRecord links[2] = {{1, 2}, {2, 3}};
    return galaxy_load(v, width, height, planets, 3, links, 2, &kSprites);
}

void test_defaults_and_load() {
    GalaxyView* v = &g_view;
    galaxy_init(v);
    CHECK(v->loading && v->locked && !v->has_data);
    CHECK(v->scale == 0 && v->view_width == 1992 && v->view_height == 1312);
    CHECK(
        v->scroll_step[0] == 20 && v->scroll_step[1] == 14 && v->scroll_step[2] == 10 &&
        v->scroll_step[3] == 3
    );
    CHECK(v->selected == kNoPlanet && v->hovered == kNoPlanet && v->territory.planet == kNoPlanet);

    CHECK(!load_three(v, 1991, 1600)); // map image smaller than the galaxy
    CHECK(load_three(v));
    CHECK(v->planet_count == 3 && v->connector_count == 2);
    CHECK(v->planets[0].id == 1 && v->planets[1].id == 2 && v->planets[2].id == 3);
    CHECK_STR(v->planets[0].name, "Core Prime");
    CHECK(v->max_scroll_x == 408 && v->max_scroll_y == 288);
    const GalaxyPlanet& e = v->planets[1];
    CHECK(e.left == 92 && e.top == 92 && e.right == 107 && e.bottom == 107);
    CHECK(
        e.map_source == MapSource::ta_disk &&
        v->planets[0].map_source == MapSource::core_contingency
    );
    CHECK(v->planets[2].map_source == MapSource::web_core_contingency);
    CHECK(e.player_limit == 4 && e.commander_death == 2);
    CHECK_STR(e.pan_gaf, "anims\\pan.gaf");
    CHECK(e.radius == 20 && e.frame_count == 1);
    CHECK_STR(e.description, "Lore");
    CHECK(e.visible && e.hotspot);
    CHECK(e.view_x == 25 && e.view_y == 25 && e.view_left == 23 && e.view_width == 4);
    CHECK(v->connectors[0].a == 0 && v->connectors[0].b == 1);
    CHECK(v->connectors[0].color == connector_color::mixed); // Core-Arm
    CHECK(v->connectors[1].color == connector_color::arm);   // Arm-Contested
    CHECK(v->connectors[0].visible && v->connectors[0].view_bx == 25);
    CHECK(v->loading && !v->has_data); // until a feed arrives

    // A duplicate id and an unknown connector end reject the map.
    const PlanetRecord twice[2] = {
        planet(5, "A", 0, 0, 'A', "TA"), planet(5, "B", 0, 0, 'A', "TA")
    };
    CHECK(!galaxy_load(v, 2400, 1600, twice, 2, nullptr, 0, nullptr));
    const PlanetRecord one[1] = {planet(5, "A", 0, 0, 'A', "TA")};
    const ConnectorRecord dangling[1] = {{5, 6}};
    CHECK(!galaxy_load(v, 2400, 1600, one, 1, dangling, 1, nullptr));
    CHECK(galaxy_load(v, 2400, 1600, one, 1, nullptr, 0, nullptr)); // no sprites: anchor only
    CHECK(v->planets[0].left == 0 && v->planets[0].right == 0 && v->planets[0].frame_count == 0);

    // An unknown variant group rejects the map; the description is the
    // planet's own bounded copy.
    PlanetRecord odd = planet(6, "Odd", 0, 0, 'A', "TA");
    odd.variant_group = 3;
    CHECK(!galaxy_load(v, 2400, 1600, &odd, 1, nullptr, 0, nullptr));
    std::string lore(kPlanetDescriptionCapacity + 16, 'x');
    PlanetRecord told = planet(7, "Told", 0, 0, 'A', "TA");
    told.description = lore.c_str();
    CHECK(galaxy_load(v, 2400, 1600, &told, 1, nullptr, 0, nullptr));
    lore.assign(lore.size(), 'y');
    CHECK(std::strlen(v->planets[0].description) == kPlanetDescriptionCapacity - 1);
    CHECK(v->planets[0].description[0] == 'x');
}

void test_feeds_scroll_and_scale() {
    GalaxyView* v = &g_view;
    galaxy_init(v);
    CHECK(load_three(v));
    galaxy_scroll(v, 1, 0, false);
    CHECK(v->scroll_x == 0); // ignored while loading

    galaxy_apply_allegiances(v, "CCA");
    CHECK(v->has_data && !v->loading);
    CHECK(v->planets[1].side == 'C' && v->planets[2].side == 'A');
    CHECK(v->connectors[0].color == connector_color::core);  // Core-Core
    CHECK(v->connectors[1].color == connector_color::mixed); // Core-Arm
    galaxy_apply_activity(v, "37");
    CHECK(
        v->planets[0].activity == 3 && v->planets[1].activity == 7 && v->planets[2].activity == 0
    );

    galaxy_scroll(v, 1, 0, false);
    CHECK(v->scroll_x == 20 && v->scroll_y == 0);
    const GalaxyPlanet& core = v->planets[0]; // x 10: sprite 2..17 now left of the view
    CHECK(!core.visible && !core.hotspot);
    const GalaxyPlanet& emp = v->planets[1];
    CHECK(emp.visible && emp.view_x == 20 && emp.view_left == 18 && emp.view_width == 4);
    CHECK(v->connectors[0].visible); // one end outside, the other inside
    galaxy_scroll(v, -5, 0, false);
    CHECK(v->scroll_x == 0 && core.visible && core.hotspot);
    galaxy_scroll(v, 100, 100, false);
    CHECK(v->scroll_x == 408 && v->scroll_y == 288);
    CHECK(!v->connectors[0].visible); // both ends left of the view
    galaxy_scroll(v, -408, -288, true);
    CHECK(v->scroll_x == 0 && v->scroll_y == 0);

    uint32_t area = 0;
    CHECK(!galaxy_select_planet(v, 1, &area)); // locked
    CHECK(!galaxy_hover_planet(v, 1) && v->hovered == kNoPlanet);
    v->locked = false;
    v->territory_attached = true;
    CHECK(galaxy_select_planet(v, 1, &area));
    CHECK(area == 2 && v->selected == 1 && v->territory.planet == 1);
    CHECK(galaxy_hover_planet(v, 2) && v->hovered == 2);
    CHECK(galaxy_hover_planet(v, kNoPlanet) && v->hovered == kNoPlanet);
    CHECK(v->territory.shown && !v->territory.contested_shown);
    CHECK_STR(v->territory.name, "Empyrrean");
    CHECK_STR(v->territory.owner, "Core"); // second character of the "CCA" feed

    galaxy_scroll(v, 1, 0, false); // scroll 20, 0: Empyrrean at view (20, 25)
    galaxy_set_scale(v, 3);
    CHECK(v->view_width == 498 && v->view_height == 328);
    CHECK(v->max_scroll_x == 1902 && v->max_scroll_y == 1272);
    CHECK(v->scroll_x == 80 && v->scroll_y == 75);
    CHECK(emp.view_x == 20 && emp.view_y == 25); // kept in place
    CHECK(emp.view_width == 16 && emp.view_left == 12);
    galaxy_set_scale(v, 0);
    CHECK(v->scroll_x == 20 && v->scroll_y == 0);
    galaxy_set_scale(v, 4);
    CHECK(v->scale == 0);
}

void test_census_and_territory() {
    GalaxyView* v = &g_view;
    galaxy_init(v);
    CHECK(load_three(v));
    galaxy_apply_allegiances(v, "CAI");
    v->locked = false;
    v->territory_attached = true;
    uint32_t area = 0;
    galaxy_select_planet(v, 2, &area);
    CHECK(area == 3 && v->territory.contested_shown);
    CHECK_STR(v->territory.owner, "Contested");
    CensusReport census{3, 12, 5, 2, 1, 9, 4};
    CHECK(galaxy_apply_census(v, &census, 1)); // another area: no panel refresh
    const GalaxyPlanet& b = v->planets[2];
    CHECK(b.population == 12 && b.activity == 5 && b.in_progress == 2 && b.forming == 1);
    CHECK(b.core_wins == 9 && b.arm_wins == 4);
    CHECK(v->territory.in_progress.count == 0);
    galaxy_apply_census(v, &census, 3);
    CHECK(v->territory.in_progress.count == 2 && v->territory.forming.count == 1);
    CHECK_STR(v->territory.arm_wins.text, "4");
    CHECK_STR(v->territory.core_wins.text, "9");
    CensusReport unknown{99, 1, 1, 1, 1, 1, 1};
    CHECK(!galaxy_apply_census(v, &unknown, 99));
    v->territory.planet = kNoPlanet;
    territory_panel_update(&v->territory, v);
    CHECK(!v->territory.shown && !v->territory.contested_shown);
}

void test_planet_info() {
    GalaxyView* v = &g_view;
    galaxy_init(v);
    CHECK(load_three(v));
    Tdf tdf;
    const MapTdfReader reader{&tdf, tdf_open, tdf_int};
    static PlanetInfo info;
    GalaxyPlanet& emp = v->planets[1];
    emp.population = 3;
    planet_info_fill(&info, &emp, &reader, fixed_random, nullptr);
    CHECK_STR(tdf.opened.c_str(), "Coast to Coast");
    CHECK_STR(info.labels[planet_info_row::planet], "Planet:");
    CHECK_STR(info.labels[planet_info_row::population], "Population:");
    CHECK_STR(info.values[planet_info_row::planet], "Empyrrean");
    CHECK_STR(info.values[planet_info_row::gw_map], "Coast to Coast");
    CHECK_STR(info.values[planet_info_row::type], "Green");
    CHECK_STR(info.values[planet_info_row::gravity], "112");
    CHECK(info.wind_countdown == 7);
    CHECK_STR(info.values[planet_info_row::wind_speed], "17"); // 7 % 30 + 10
    CHECK_STR(info.values[planet_info_row::tide_strength], "20");
    CHECK_STR(info.values[planet_info_row::affiliation], "Arm");
    CHECK_STR(info.values[planet_info_row::population], "3");
    CHECK(info.map_installed && info.notice_count == 0);
    CHECK_STR(info.description, "Lore");

    tdf.lavaworld = 1;
    tdf.min_wind = tdf.max_wind = 5;
    emp.environment.loaded = false;
    planet_info_fill(&info, &emp, &reader, fixed_random, nullptr);
    CHECK_STR(info.values[planet_info_row::wind_speed], "5");
    CHECK_STR(info.values[planet_info_row::tide_strength], "n/a");
    emp.environment.loaded = false;
    planet_info_fill(&info, &emp, &reader, fixed_random, nullptr); // cached: no re-read
    tdf.installed = false;
    GalaxyPlanet& bar = v->planets[2];
    planet_info_fill(&info, &bar, &reader, fixed_random, nullptr);
    CHECK(!info.map_installed);
    CHECK_STR(info.values[planet_info_row::gravity], "?");
    CHECK_STR(info.values[planet_info_row::tide_strength], "?");
    CHECK_STR(info.values[planet_info_row::affiliation], "Contested");
    CHECK(info.notice_count > 0);
    bool mentions_cc = false;
    for (std::size_t i = 0; i < info.notice_count; ++i)
        mentions_cc = mentions_cc || std::strstr(info.notice[i], "Core Contingency") != nullptr;
    CHECK(mentions_cc);
    planet_info_fill(&info, &bar, nullptr, nullptr, nullptr);
    CHECK(!info.map_installed);
}

// The loading static cycles through its frames; once map data is present
// each planet sprite steps on every frame_interval-th animation step.
void test_animation() {
    GalaxyView* v = &g_view;
    galaxy_init(v);
    CHECK(v->static_frame == 0 && v->animation_steps == 0 && !v->show_map_while_loading);
    const int32_t cycle[] = {1, 2, 0, 1};
    for (int32_t frame : cycle) {
        galaxy_step_animation(v, 3);
        CHECK(v->static_frame == frame);
    }
    CHECK(v->animation_steps == 0); // no map data yet

    PlanetRecord records[2] = {
        planet(1, "Core Prime", 10, 100, 'C', "TA"), planet(2, "Empyrrean", 100, 100, 'A', "TA")
    };
    records[1].frame_interval = 2;
    const SpriteFrame frames[5] = {
        {16, 16, 8, 8}, {16, 16, 8, 8}, {16, 16, 8, 8}, {16, 16, 8, 8}, {16, 16, 8, 8}
    };
    const std::size_t counts[2] = {3, 2};
    const PlanetSprites sprites{frames, counts};
    CHECK(galaxy_load(v, 2400, 1600, records, 2, nullptr, 0, &sprites));
    CHECK(v->planets[0].frame_count == 3 && v->planets[1].frame_count == 2);
    galaxy_apply_allegiances(v, "CA");
    const int32_t every_step[] = {1, 2, 0};
    const int32_t every_other[] = {1, 1, 0};
    for (int32_t i = 0; i < 3; ++i) {
        galaxy_step_animation(v, 3);
        CHECK(v->planets[0].frame == every_step[i] && v->planets[1].frame == every_other[i]);
    }
    CHECK(v->static_frame == 1 && v->animation_steps == 3); // the static stopped
    galaxy_clear(v);
    CHECK(v->loading && v->animation_steps == 0);
}

// While the map loads, the war map is drawn only when show_map_while_loading
// is set and map data is present. The flag starts clear and clearing the
// view clears it, so the loading picture is what comes up.
void test_show_map_while_loading() {
    GalaxyView* v = &g_view;
    galaxy_init(v);
    CHECK(v->loading && !v->has_data && !v->show_map_while_loading);
    CHECK(!galaxy_shows_war_map(v));
    v->show_map_while_loading = true;
    CHECK(!galaxy_shows_war_map(v)); // no map data yet
    v->has_data = true;
    CHECK(galaxy_shows_war_map(v));
    v->show_map_while_loading = false;
    CHECK(!galaxy_shows_war_map(v)); // the loading picture stays up
    v->loading = false;
    CHECK(galaxy_shows_war_map(v));
    galaxy_clear(v);
    CHECK(!v->show_map_while_loading && v->loading && !galaxy_shows_war_map(v));
}

/// Checks the star layers of a 2400 x 1600 map in sector 0: 120-unit cells (5% of the width,
/// 20 x 14 of them) seeded 0, 1 and 2, each star placed, shaped and dimmed by the rand15
/// generator as the layer builder and the star placer draw it, and each layer's window following
/// its share of the scroll range (the stars 3.1c draws for the same map).
void test_star_layers() {
    GalaxyView* v = &g_view;
    galaxy_init(v);
    CHECK(load_three(v));
    for (const GalaxyStarLayer& layer : v->star_layers)
        CHECK(layer.star_count == 20 * 14 && layer.width == 2400 && layer.height == 1600);
    const GalaxyStarLayer& near = v->star_layers[0];
    const GalaxyStarLayer& mid = v->star_layers[1];
    const GalaxyStarLayer& far = v->star_layers[2];
    CHECK(near.scroll_share == 0.4f && mid.scroll_share == 0.2f && far.scroll_share == 0.0f);
    CHECK(near.dim == 0 && mid.dim == 2 && far.dim == 4);
    const GalaxyStar& first = near.stars[0];
    CHECK(first.x == 38 && first.y == 103 && first.kind == StarKind::point && first.dim == 2);
    CHECK(first.right == 38 && first.bottom == 103);
    const GalaxyStar& cross = far.stars[0];
    CHECK(cross.kind == StarKind::cross && cross.x == 45 && cross.y == 61 && cross.dim == 6);
    CHECK(cross.right == 47 && cross.bottom == 63);
    const GalaxyStar& pair = mid.stars[150];
    CHECK(pair.kind == StarKind::pair && pair.x == 1260 && pair.y == 916);
    // A spot below the image in the last row leaves its cell empty: a star
    // of no kind at the map corner, which the corner window counts as
    // visible but which draws nothing.
    const GalaxyStar& empty = near.stars[261];
    CHECK(empty.kind == StarKind::none && empty.x == 0 && empty.y == 0 && empty.visible);

    // Level 0 at the corner: the windows are 2238 x 1486, 2320 x 1544 and
    // the whole map.
    CHECK(first.visible && first.view_x == 8 && first.view_y == 22);
    CHECK(near.stars[150].view_x == 276 && near.stars[150].view_y == 189);
    CHECK(far.stars[150].view_x == 258 && far.stars[150].view_y == 173);
    galaxy_scroll(v, 100, 100, false);
    CHECK(v->scroll_x == 0 && first.view_x == 8); // ignored while loading

    galaxy_apply_allegiances(v, "CCA");
    galaxy_scroll(v, 100, 100, false); // clamped to 408, 288
    CHECK(!first.visible && first.view_x == -27 && first.view_y == -2);
    CHECK(near.stars[150].view_x == 240 && near.stars[150].view_y == 164);
    CHECK(pair.view_x == 253 && pair.view_y == 182);
    CHECK(far.stars[150].view_x == 258 && far.stars[150].view_y == 173); // the far layer stays

    galaxy_scroll(v, -328, -213, true);
    CHECK(v->scroll_x == 80 && v->scroll_y == 75);
    galaxy_set_scale(v, 3);
    CHECK(v->scroll_x == 80 && v->scroll_y == 75); // nothing selected
    CHECK(first.visible && first.view_x == 2 && first.view_y == 22);
    CHECK(near.stars[150].view_x == 367 && near.stars[150].view_y == 249);
    CHECK(mid.stars[0].view_x == 6 && mid.stars[0].view_y == 1);
    galaxy_scroll(v, 10000, 10000, true);
    CHECK(v->scroll_x == 1902 && v->scroll_y == 1272);
    CHECK(!first.visible && first.view_x == -219 && first.view_y == -121);
    CHECK(near.stars[150].view_x == 146 && near.stars[150].view_y == 105);
    CHECK(pair.view_x == 217 && pair.view_y == 161);
    CHECK(far.stars[150].view_x == 258 && far.stars[150].view_y == 173);

    galaxy_clear(v);
    CHECK(v->star_layers[0].star_count == 0 && v->star_layers[2].star_count == 0);
    // 30000 rows of 120-unit cells need more than kMaxGalaxyStars stars.
    CHECK(!load_three(v, 2400, 30000));

    // Straight from the generator seeded 0 (first draws 38, 7719, 21238):
    // a point dims 38 % 6 * 2; an unknown kind becomes a point dimmed
    // 38 % 6, undoubled; a pair takes its variant, then its dim.
    uint32_t holdrand = 0;
    GalaxyStar star{};
    galaxy_place_star(&star, 5, 6, StarKind::point, &holdrand);
    CHECK(star.kind == StarKind::point && star.dim == 4 && star.variant == 0);
    holdrand = 0;
    galaxy_place_star(&star, 5, 6, StarKind::none, &holdrand);
    CHECK(star.kind == StarKind::point && star.dim == 2 && star.right == 5 && star.bottom == 6);
    holdrand = 0;
    galaxy_place_star(&star, 5, 6, StarKind::pair, &holdrand);
    CHECK(star.variant == 2 && star.dim == 6 && star.right == 5 && star.bottom == 7);
    galaxy_project_star(&star, 5, 7, 498, 328);
    CHECK(star.visible && star.view_x == 0 && star.view_y == -1);
    galaxy_project_star(&star, 6, 6, 498, 328);
    CHECK(!star.visible);
}

// The war-map feeds reach the navigator's galaxy view through its event
// sink; a census for the current area refreshes the territory summary.
void test_navigator_feeds() {
    static BoneyardsScreens nav;
    boneyards_screens_init(&nav, nullptr, SettingsStore{});
    CHECK(load_three(&nav.galaxy));
    const oa::boneyards::ServiceEvents events = boneyards_screens_events(&nav);
    CHECK(events.allegiances != nullptr && events.activity != nullptr && events.census != nullptr);
    events.allegiances(events.user, "CAI");
    CHECK(!nav.galaxy.loading && nav.galaxy.has_data);
    CHECK(
        nav.galaxy.planets[0].side == 'C' && nav.galaxy.planets[1].side == 'A' &&
        nav.galaxy.planets[2].side == 'I'
    );
    events.activity(events.user, "408");
    CHECK(nav.galaxy.planets[0].activity == 4 && nav.galaxy.planets[2].activity == 8);

    nav.galaxy.locked = false;
    nav.galaxy.territory_attached = true;
    CHECK(galaxy_select_planet(&nav.galaxy, 2, &nav.session.area_id) && nav.session.area_id == 3);
    const CensusReport census{3, 30, 2, 5, 1, 7, 6};
    events.census(events.user, &census);
    const GalaxyPlanet& contested = nav.galaxy.planets[2];
    CHECK(contested.population == 30 && contested.in_progress == 5 && contested.forming == 1);
    CHECK(contested.core_wins == 7 && contested.arm_wins == 6);
    CHECK(nav.galaxy.territory.in_progress.count == 5);
    CHECK_STR(nav.galaxy.territory.arm_wins.text, "6");
    CHECK_STR(nav.galaxy.territory.core_wins.text, "7");
    const CensusReport elsewhere{99, 1, 1, 1, 1, 1, 1};
    CHECK(!boneyards_on_census(&nav, &elsewhere));
    const CensusReport other{1, 12, 0, 9, 0, 0, 0};
    CHECK(boneyards_on_census(&nav, &other));
    CHECK(nav.galaxy.planets[0].in_progress == 9 && nav.galaxy.territory.in_progress.count == 5);
}

/// Checks the shared sequence after seeding with 1 (x = x * 214013 + 2531011, bits 16-30)
/// and a private state stepped beside it.
void test_rand15() {
    const int32_t expected[] = {41, 18467, 6334, 26500, 19169, 15724, 11478, 29358, 26962, 24464};
    rand15_seed(1);
    for (int32_t value : expected)
        CHECK(rand15() == value);
    uint32_t holdrand = 1;
    CHECK(rand15_step(&holdrand) == 41 && holdrand == 0x0029e2c0u);
    CHECK(rand15() == 5705); // the process stream is untouched by a private one
    rand15_seed(1);
    CHECK(rand15_source(nullptr) == 41);
}

// Values handed out in order, then repeated.
struct Script {
    const int32_t* values;
    std::size_t count;
    std::size_t next;
};

int32_t scripted(void* user) {
    auto* s = static_cast<Script*>(user);
    return s->values[s->next++ % s->count];
}

void test_recon_step() {
    static GalaxyPlanet planet;
    static PlanetInfo info;
    planet = GalaxyPlanet{};
    info = PlanetInfo{};
    planet.environment.loaded = true;
    planet.environment.min_wind = 10;
    planet.environment.max_wind = 12;
    info.map_installed = true;
    info.wind_countdown = 1;
    info.wind_speed = 11;
    const int32_t widths[] = {100, 150, 90};
    ReconState r;
    recon_begin(&r, 4, &planet, &info, widths, 3);
    CHECK(r.planet == 4 && r.map_installed && r.has_pan);
    CHECK(r.pan_frame_count == 3 && r.pan_total == 340);
    CHECK(r.pan_offsets[0] == 0 && r.pan_offsets[1] == 100 && r.pan_offsets[2] == 250);
    CHECK(r.pan_scroll == 0 && r.pan_end == 0x1d6);
    CHECK(r.wind == 11 && r.wind_min == 10 && r.wind_max == 12 && r.wind_range == 2);
    CHECK(r.wind_countdown == 1 && !r.wind_dirty);

    // Expiry: 4 % 5 - 2 lifts the wind past its maximum, where it clamps,
    // and 88 % 81 reseeds the countdown.
    const int32_t rise[] = {4, 88};
    Script script{rise, 2, 0};
    recon_step(&r, scripted, &script, 3);
    CHECK(r.wind == 12 && r.wind_dirty && r.wind_countdown == 7 && script.next == 2);
    CHECK(r.pan_scroll == 1 && r.pan_end == (1 + 0x1d6) % 340);
    r.wind_dirty = false;
    for (int32_t i = 0; i < 6; ++i)
        recon_step(&r, scripted, &script, 3);
    CHECK(r.wind_countdown == 1 && script.next == 2 && !r.wind_dirty);

    // 0 % 5 - 2 drops the wind by two, to its minimum, then clamps there.
    const int32_t fall[] = {0, 1};
    script = Script{fall, 2, 0};
    recon_step(&r, scripted, &script, 3);
    CHECK(r.wind == 10 && r.wind_dirty && r.wind_countdown == 1);
    recon_step(&r, scripted, &script, 3);
    CHECK(r.wind == 10 && r.wind_countdown == 1 && script.next == 4);

    // The strip wraps at its total width.
    r.pan_scroll = r.pan_total - 1;
    recon_step(&r, scripted, &script, 3);
    CHECK(r.pan_scroll == 0 && r.pan_end == 0x1d6 % 340);
    CHECK(r.static_frame == 0);

    // A still wind never counts down.
    r.wind_range = 0;
    const std::size_t draws = script.next;
    for (int32_t i = 0; i < 5; ++i)
        recon_step(&r, scripted, &script, 3);
    CHECK(script.next == draws && r.wind_countdown == 1);

    // Without a random source the countdown still expires but the wind stays.
    r.wind_range = 2;
    r.wind_dirty = false;
    recon_step(&r, nullptr, nullptr, 3);
    recon_step(&r, nullptr, nullptr, 3);
    CHECK(r.wind_countdown == -1 && r.wind == 10 && !r.wind_dirty);

    // Without the map the static cycles through its frames and the strip
    // stays put.
    info.map_installed = false;
    recon_begin(&r, 4, &planet, &info, widths, 3);
    CHECK(!r.map_installed && r.has_pan && r.wind == 0 && r.wind_range == 0);
    const int32_t cycle[] = {1, 2, 0, 1};
    for (int32_t frame : cycle) {
        recon_step(&r, scripted, &script, 3);
        CHECK(r.static_frame == frame);
    }
    CHECK(r.pan_scroll == 0);

    // Nor does an installed map without its pan sequence.
    info.map_installed = true;
    recon_begin(&r, 4, &planet, &info, nullptr, 0);
    CHECK(r.map_installed && !r.has_pan && r.pan_total == 0 && r.static_frame == 0);
    recon_step(&r, scripted, &script, 3);
    CHECK(r.static_frame == 1 && r.pan_scroll == 0);
}

} // namespace

int main() {
    test_rand15();
    test_recon_step();
    test_defaults_and_load();
    test_animation();
    test_show_map_while_loading();
    test_feeds_scroll_and_scale();
    test_census_and_territory();
    test_planet_info();
    test_navigator_feeds();
    test_star_layers();
    return by_test::finish("boneyards-galaxy");
}
