// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The galaxy load over an install's war map (the alpha_map.gwm of the
// multiplayer install OA_LAUNCHER_GAME_DIR names, or the byte-identical
// BYPacific_map.gwm of OA_GAME_DIR's BY1117a) read through oa-rover's
// sector-map slots, with the planet sprites of its by.ccx.
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "galaxy_graphics.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/boneyards/rover_calls.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/toolkit.hpp"
#include "oa/rover/platform.h"

using namespace oa::boneyards;
using namespace oa::boneyards::console;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr int32_t kSectorId = 45;
constexpr int32_t kFirstPlanet = 1796;
constexpr std::size_t kPlanetCount = 25;
constexpr std::size_t kConnectorCount = 32;
constexpr const char* kAllegiances = "CCCCIIIAAACCCCCCAAAAAAAAC";
constexpr const char* kFuelDumpLore = "Like Abomination, this world was constructed by the CORE";

// What the load asked of the service, in order, and how to disturb it.
struct Recorder {
    BoneyardsService rover;
    std::vector<std::string> calls;
    bool fail_open = false;
    int32_t width_override = 0;
    bool duplicate_id = false;
};

Recorder* recorder(void* user) {
    return static_cast<Recorder*>(user);
}

BoneyardsService recording_service(Recorder* r) {
    BoneyardsService s{};
    s.user = r;
    s.request_allegiance = [](void* user) { recorder(user)->calls.emplace_back("allegiance"); };
    s.request_census = [](void* user, uint32_t area) {
        recorder(user)->calls.push_back("census(" + std::to_string(area) + ")");
    };
    s.open_sector_map = [](void* user, const char* path, SectorMapHeader* header) {
        Recorder* r = recorder(user);
        r->calls.emplace_back("open");
        if (r->fail_open)
            return static_cast<SectorMapReader*>(nullptr);
        SectorMapReader* reader = r->rover.open_sector_map(r->rover.user, path, header);
        if (reader != nullptr && r->width_override != 0)
            header->width = r->width_override;
        return reader;
    };
    s.read_sector_map = [](void* user,
                           SectorMapReader* reader,
                           int32_t width,
                           int32_t height,
                           SectorPlanetRecord* planets,
                           int32_t planet_count,
                           ConnectorRecord* connectors,
                           int32_t connector_count) {
        Recorder* r = recorder(user);
        r->calls.emplace_back("read");
        const bool read = r->rover.read_sector_map(
            r->rover.user, reader, width, height, planets, planet_count, connectors, connector_count
        );
        if (read && r->duplicate_id && planet_count > 1)
            planets[1].id = planets[0].id;
        return read;
    };
    s.close_sector_map = [](void* user, SectorMapReader* reader) {
        Recorder* r = recorder(user);
        r->calls.emplace_back("close");
        r->rover.close_sector_map(r->rover.user, reader);
    };
    boneyards_service_fill_offline(&s);
    return s;
}

struct Store {
    oa::AssetStore assets;
};

bool read_asset(void* user, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<Store*>(user)->assets.read(path).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

using Calls = std::vector<std::string>;

void expect_named(const screens::GalaxyView& view, int32_t index, int32_t id, const char* name) {
    CHECK(index == screens::galaxy_find_planet(&view, id));
    if (index < 0 || index >= static_cast<int32_t>(view.planet_count))
        return;
    CHECK(std::strcmp(view.planets[index].name, name) == 0);
}

// Arrows on the loaded war map, and each side's capitol. The link between
// DP Alpha and Core Prime II is dropped only in the copy the direction
// function reads.
void test_keyboard(screens::GalaxyView& view) {
    CHECK(nearest_planet(nullptr, nullptr, nullptr, 1, 0, key::right) == -1);
    CHECK(screens::galaxy_player_capitol(nullptr, 'C') == screens::kNoPlanet);
    constexpr int32_t count = 25;
    CHECK(view.planet_count == static_cast<std::size_t>(count));
    CHECK(view.connector_count == kConnectorCount);
    if (view.planet_count != static_cast<std::size_t>(count))
        return;
    int32_t xs[count];
    int32_t ys[count];
    uint8_t linked[count * count]{};
    for (int32_t i = 0; i < count; ++i) {
        xs[i] = view.planets[i].x;
        ys[i] = view.planets[i].y;
    }
    for (std::size_t link = 0; link < view.connector_count; ++link) {
        const int32_t a = view.connectors[link].a;
        const int32_t b = view.connectors[link].b;
        if (a < 0 || b < 0 || a >= count || b >= count)
            continue;
        linked[a * count + b] = 1;
        linked[b * count + a] = 1;
    }
    CHECK(linked[0 * count + 1] == 1);
    const int32_t alpha = 0;
    expect_named(
        view, nearest_planet(xs, ys, linked, count, alpha, key::right), 1797, "Core Prime II"
    );
    expect_named(view, nearest_planet(xs, ys, linked, count, alpha, key::down), 1808, "Lusch II");
    CHECK(nearest_planet(xs, ys, linked, count, alpha, key::left) == -1);
    CHECK(nearest_planet(xs, ys, linked, count, alpha, key::up) == -1);
    CHECK(nearest_planet(xs, ys, linked, count, alpha, key::enter) == -1);
    const int32_t lusch = screens::galaxy_find_planet(&view, 1808);
    CHECK(lusch >= 0);
    if (lusch >= 0) {
        expect_named(view, nearest_planet(xs, ys, linked, count, lusch, key::down), 1801, "Slag");
        expect_named(view, nearest_planet(xs, ys, linked, count, lusch, key::right), 1807, "Koth");
        expect_named(view, nearest_planet(xs, ys, linked, count, lusch, key::up), 1796, "DP Alpha");
        CHECK(nearest_planet(xs, ys, linked, count, lusch, key::left) == -1);
    }
    linked[0 * count + 1] = 0;
    linked[1 * count + 0] = 0;
    expect_named(view, nearest_planet(xs, ys, linked, count, alpha, key::right), 1807, "Koth");

    const int32_t core = screens::galaxy_player_capitol(&view, 'C');
    expect_named(view, core, 1797, "Core Prime II");
    CHECK(screens::galaxy_player_capitol(&view, 'c') == core);
    const int32_t arm = screens::galaxy_player_capitol(&view, 'A');
    expect_named(view, arm, 1819, "New Empyrrean");
    CHECK(screens::galaxy_player_capitol(&view, 'a') == arm);
    CHECK(screens::galaxy_player_capitol(&view, 'U') == screens::kNoPlanet);
    if (core >= 0) {
        const char saved = view.planets[core].side;
        view.planets[core].side = 'A';
        CHECK(screens::galaxy_player_capitol(&view, 'C') == core);
        view.planets[core].side = saved;
    }
}

void test_load(GalaxyGraphics& graphics, Recorder& r, const char* path) {
    auto view = std::make_unique<screens::GalaxyView>();
    screens::galaxy_init(view.get());
    const BoneyardsService service = recording_service(&r);
    CHECK(galaxy_view_load(view.get(), graphics, service, path));
    CHECK((r.calls == Calls{"allegiance", "census(0)", "open", "read", "close"}));
    CHECK(view->sector_id == kSectorId && std::strcmp(view->sector_name, "New Hub") == 0);
    CHECK(view->map_width == 2000 && view->map_height == 1400);
    CHECK(
        view->max_scroll_x == 2000 - view->view_width &&
        view->max_scroll_y == 1400 - view->view_height
    );
    CHECK(view->planet_count == kPlanetCount && view->connector_count == kConnectorCount);
    bool ascending = view->planet_count == kPlanetCount;
    bool sprites = true;
    bool sequences = true;
    for (std::size_t i = 0; i < view->planet_count; ++i) {
        const screens::GalaxyPlanet& p = view->planets[i];
        ascending = ascending && p.id == kFirstPlanet + static_cast<int32_t>(i);
        sprites = sprites && p.frame_count > 0 && p.left < p.x && p.x < p.right && p.top < p.y &&
                  p.y < p.bottom;
        const auto& sizes = graphics.planet_sequences[i];
        for (const oa::formats::gaf::Sequence* sequence : sizes)
            sequences = sequences && sequence != nullptr &&
                        sequence->frames.size() == static_cast<std::size_t>(p.frame_count);
    }
    CHECK(ascending);
    CHECK(sprites);
    CHECK(sequences);
    CHECK(graphics.planet_sequences[kPlanetCount][0] == nullptr);
    const int32_t fuel_dump = screens::galaxy_find_planet(view.get(), 1803);
    CHECK(fuel_dump != screens::kNoPlanet);
    if (fuel_dump != screens::kNoPlanet) {
        const screens::GalaxyPlanet& p = view->planets[fuel_dump];
        CHECK(std::strcmp(p.name, "Fuel Dump") == 0 && p.x == 991 && p.y == 849);
        CHECK(std::strncmp(p.description, kFuelDumpLore, std::strlen(kFuelDumpLore)) == 0);
        CHECK(p.size_class == 0 && p.frame_interval == 5 && p.player_limit == 4);
        CHECK(std::strcmp(p.pan_gaf, "anims\\Metalbrief.gaf") == 0);
        CHECK(
            std::strcmp(graphics.planet_sequences[fuel_dump][0]->name.c_str(), "Metal_25_L") == 0
        );
        CHECK(
            std::strcmp(graphics.planet_sequences[fuel_dump][3]->name.c_str(), "Metal_100_L") == 0
        );
    }
    CHECK(view->loading && !view->has_data);

    screens::galaxy_apply_allegiances(view.get(), kAllegiances);
    CHECK(!view->loading && view->has_data);
    bool sides = true;
    for (std::size_t i = 0; i < kPlanetCount; ++i)
        sides = sides && view->planets[i].side == kAllegiances[i];
    CHECK(sides);
    const CensusReport census{1801, 17, 3, 2, 1, 6, 5};
    CHECK(screens::galaxy_apply_census(view.get(), &census, 0));
    const screens::GalaxyPlanet& planet = view->planets[1801 - kFirstPlanet];
    CHECK(planet.id == 1801 && planet.population == 17 && planet.in_progress == 2);
    CHECK(planet.core_wins == 6 && planet.arm_wins == 5);
    test_keyboard(*view);
}

void test_failures(GalaxyGraphics& graphics, Recorder& r, const char* path) {
    auto view = std::make_unique<screens::GalaxyView>();
    screens::galaxy_init(view.get());
    const BoneyardsService service = recording_service(&r);

    r.calls.clear();
    r.fail_open = true;
    CHECK(!galaxy_view_load(view.get(), graphics, service, path));
    CHECK((r.calls == Calls{"allegiance", "census(0)", "open"}));
    r.fail_open = false;

    r.calls.clear();
    r.width_override = 0x7c7;
    CHECK(!galaxy_view_load(view.get(), graphics, service, path));
    CHECK((r.calls == Calls{"allegiance", "census(0)", "open", "close"}));
    CHECK(view->planet_count == 0 && view->loading);
    r.width_override = 0;

    r.calls.clear();
    r.duplicate_id = true;
    CHECK(!galaxy_view_load(view.get(), graphics, service, path));
    CHECK((r.calls == Calls{"allegiance", "census(0)", "open", "read", "close"}));
    r.duplicate_id = false;

    r.calls.clear();
    CHECK(!galaxy_view_load(view.get(), graphics, service, "Boneyards\\missing_map.gwm"));
    CHECK((r.calls == Calls{"allegiance", "census(0)", "open"}));

    // Without the planet sprites no planet's sequences resolve.
    GalaxyGraphics bare;
    r.calls.clear();
    CHECK(!galaxy_view_load(view.get(), bare, service, path));
    CHECK((r.calls == Calls{"allegiance", "census(0)", "open", "read", "close"}));
}

} // namespace

int main() {
    struct Install {
        fs::path root;
        const char* map;
    };

    const Install installs[] = {
        {oa::boneyards_test::launcher_install(), "Boneyards\\alpha_map.gwm"},
        {oa::boneyards_test::by_install(), "Boneyards\\BYPacific_map.gwm"},
    };
    const Install* install = nullptr;
    for (const Install& candidate : installs) {
        std::string relative = candidate.map;
        relative[relative.find('\\')] = '/';
        if (!candidate.root.empty() && fs::exists(candidate.root / relative) &&
            fs::exists(candidate.root / "by.ccx")) {
            install = &candidate;
            break;
        }
    }
    if (install == nullptr)
        oa::test::skip_test(
            "the galaxy loader tests",
            "neither OA_LAUNCHER_GAME_DIR nor OA_GAME_DIR names an install with a Boneyards war map"
        );
    rover_session_configure(install->root.string().c_str(), "");
    if (!rover_module_load()) {
        std::fprintf(stderr, "FAIL: the Rover table was not bound\n");
        return 1;
    }
    Store store{oa::AssetStore{install->root}};
    store.assets.mount(install->root / "by.ccx");
    const FileReader files{&store, read_asset};
    auto graphics = std::make_unique<GalaxyGraphics>();
    CHECK(galaxy_load_graphics(*graphics, files));
    Recorder recorder;
    recorder.rover = rover_session_service();
    test_load(*graphics, recorder, install->map);
    test_failures(*graphics, recorder, install->map);
    rover_module_unload();
    rover_use_platform(nullptr);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("galaxy loader tests passed (%s)\n", install->map);
    return 0;
}
