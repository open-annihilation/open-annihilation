// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Recon environment rows of the alpha war map of the multiplayer install
// OA_LAUNCHER_GAME_DIR names, read from each planet's map OTA in the
// install's archives.
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "galaxy_graphics.hpp"
#include "map_tdf_reader.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/boneyards/rover_calls.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/rand15.hpp"
#include "oa/rover/platform.h"

using namespace oa::boneyards;
using namespace oa::boneyards::console;
namespace fs = std::filesystem;
namespace row = screens::planet_info_row;

namespace {

int g_failures = 0;

void check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

bool same(const char* got, const char* want) {
    return got != nullptr && std::strcmp(got, want) == 0;
}

constexpr std::size_t kPlanetCount = 25;
constexpr int32_t kFuelDump = 1803; // Over Crude Water
constexpr int32_t kSpire = 1814;    // Pincushion

bool read_asset(void* user, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<oa::AssetStore*>(user)->read(path).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Remembers the paths the reader asked for.
struct Recorder {
    FileReader files;
    std::vector<std::string> paths;
};

bool record_read(void* user, const char* path, std::vector<uint8_t>* out) {
    auto* r = static_cast<Recorder*>(user);
    r->paths.emplace_back(path);
    return r->files.read(r->files.user, path, out);
}

void test_known_planets(screens::GalaxyView* view, const FileReader& files) {
    Recorder recorder{files, {}};
    MapTdfFile* file = map_tdf_file_create(FileReader{&recorder, record_read});
    const screens::MapTdfReader reader = map_tdf_reader(file);
    auto info = std::make_unique<screens::PlanetInfo>();

    const int32_t fuel_dump = screens::galaxy_find_planet(view, kFuelDump);
    CHECK(fuel_dump != screens::kNoPlanet);
    if (fuel_dump != screens::kNoPlanet) {
        screens::GalaxyPlanet& p = view->planets[fuel_dump];
        CHECK(same(p.ta_map, "Over Crude Water"));
        screens::rand15_seed(1);
        screens::planet_info_fill(info.get(), &p, &reader, screens::rand15_source, nullptr);
        CHECK(recorder.paths.size() == 1 && recorder.paths[0] == "maps\\Over Crude Water.OTA");
        CHECK(info->map_installed && p.environment.loaded);
        CHECK(p.environment.gravity == 112 && p.environment.tidal_strength == 25);
        CHECK(p.environment.min_wind == 100 && p.environment.max_wind == 4000);
        CHECK(!p.environment.lavaworld);
        CHECK(same(info->values[row::gravity], "112"));
        CHECK(same(info->values[row::tide_strength], "25"));
        // The first two draws of the rand15 stream seeded 1 are 41 and 18467.
        CHECK(info->wind_countdown == 41 % 0x51);
        CHECK(info->wind_speed == 18467 % 3900 + 100);
        CHECK(same(info->values[row::wind_speed], "2967"));
        screens::planet_info_fill(info.get(), &p, &reader, screens::rand15_source, nullptr);
        CHECK(recorder.paths.size() == 1); // cached per planet
    }

    const int32_t spire = screens::galaxy_find_planet(view, kSpire);
    CHECK(spire != screens::kNoPlanet);
    if (spire != screens::kNoPlanet) {
        screens::GalaxyPlanet& p = view->planets[spire];
        CHECK(same(p.ta_map, "Pincushion"));
        screens::planet_info_fill(info.get(), &p, &reader, screens::rand15_source, nullptr);
        CHECK(info->map_installed && p.environment.lavaworld);
        CHECK(p.environment.min_wind == 0 && p.environment.max_wind == 3500);
        CHECK(same(info->values[row::tide_strength], "n/a"));
    }

    screens::GalaxyPlanet missing = view->planets[0];
    std::snprintf(missing.ta_map, sizeof missing.ta_map, "%s", "No Such World");
    missing.environment = {};
    screens::planet_info_fill(info.get(), &missing, &reader, screens::rand15_source, nullptr);
    CHECK(!info->map_installed && !missing.environment.loaded);
    CHECK(same(info->values[row::gravity], "?"));
    CHECK(same(info->values[row::wind_speed], "?"));
    CHECK(same(info->values[row::tide_strength], "?"));
    CHECK(info->notice_count > 0);
    map_tdf_file_destroy(file);
}

void test_every_planet(screens::GalaxyView* view, const FileReader& files) {
    MapTdfFile* file = map_tdf_file_create(files);
    const screens::MapTdfReader reader = map_tdf_reader(file);
    auto info = std::make_unique<screens::PlanetInfo>();
    std::size_t installed = 0;
    for (std::size_t i = 0; i < view->planet_count; ++i) {
        screens::GalaxyPlanet& p = view->planets[i];
        p.environment = {};
        screens::planet_info_fill(info.get(), &p, &reader, screens::rand15_source, nullptr);
        if (info->map_installed && p.environment.gravity > 0)
            ++installed;
        else
            std::fprintf(stderr, "map of %s (%s) did not resolve\n", p.name, p.ta_map);
    }
    CHECK(installed == kPlanetCount);
    map_tdf_file_destroy(file);
}

} // namespace

int main() {
    const fs::path root = oa::boneyards_test::launcher_install();
    const char* map = "Boneyards\\alpha_map.gwm";
    if (root.empty() ||
        !oa::test::netgame::holds(root, {"Boneyards/alpha_map.gwm", "by.ccx", "totala2.hpi"}))
        oa::test::skip_test(
            "the map OTA reader tests",
            "OA_LAUNCHER_GAME_DIR names no multiplayer install with Boneyards/alpha_map.gwm"
        );
    rover_session_configure(root.string().c_str(), "");
    if (!rover_module_load()) {
        std::fprintf(stderr, "FAIL: the Rover table was not bound\n");
        return 1;
    }
    oa::AssetStore assets{root};
    (void)assets.discover("31");
    const FileReader files{&assets, read_asset};
    auto graphics = std::make_unique<GalaxyGraphics>();
    CHECK(galaxy_load_graphics(*graphics, files));
    auto view = std::make_unique<screens::GalaxyView>();
    screens::galaxy_init(view.get());
    BoneyardsService service = rover_session_service();
    service.request_allegiance = [](void*) {};
    service.request_census = [](void*, uint32_t) {};
    CHECK(galaxy_view_load(view.get(), *graphics, service, map));
    CHECK(view->planet_count == kPlanetCount);
    test_known_planets(view.get(), files);
    test_every_planet(view.get(), files);
    rover_module_unload();
    rover_use_platform(nullptr);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("map OTA reader tests passed\n");
    return 0;
}
