// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A planet map is read from maps-<language>\<name>.OTA when that file opens,
// otherwise from maps\<name>.OTA. A base-disc map that fails is read again
// at that same path after the archives are scanned; a map from any other
// source is not, and a language file that appears only in that scan is not
// the one retried.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "map_tdf_reader.hpp"
#include "oa/boneyards/screens/galaxy.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {

int g_failures = 0;

void check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

const char* kMapText = "[GlobalHeader]\n"
                       "{\n"
                       "gravity=112;\n"
                       "minwindspeed=100;\n"
                       "maxwindspeed=4000;\n"
                       "tidalstrength=25;\n"
                       "lavaworld=0;\n"
                       "}\n";

const char* kNoHeader = "[Other]\n"
                        "{\n"
                        "gravity=1;\n"
                        "}\n";

struct Held {
    const char* path;
    std::vector<uint8_t> bytes;
};

struct Files {
    std::vector<Held> present;
    std::vector<Held> after_rescan;
    std::vector<std::string> asked;
    int rescans = 0;
};

std::vector<uint8_t> as_bytes(const char* text) {
    return std::vector<uint8_t>(text, text + std::strlen(text));
}

bool read_file(void* user, const char* path, std::vector<uint8_t>* out) {
    auto* files = static_cast<Files*>(user);
    files->asked.emplace_back(path);
    for (const Held& held : files->present) {
        if (std::strcmp(held.path, path) == 0) {
            *out = held.bytes;
            return true;
        }
    }
    return false;
}

void rescan(void* user) {
    auto* files = static_cast<Files*>(user);
    ++files->rescans;
    for (const Held& held : files->after_rescan)
        files->present.push_back(held);
}

screens::GalaxyPlanet planet_named(const char* map, screens::MapSource source) {
    screens::GalaxyPlanet planet{};
    std::snprintf(planet.ta_map, sizeof planet.ta_map, "%s", map);
    planet.map_source = source;
    return planet;
}

void fill(screens::PlanetInfo* info, screens::GalaxyPlanet* planet, MapTdfFile* file) {
    const screens::MapTdfReader reader = map_tdf_reader(file);
    screens::planet_info_fill(info, planet, &reader, nullptr, nullptr);
}

void test_language_folder() {
    const std::vector<uint8_t> text = as_bytes(kMapText);
    Files files;
    files.present.push_back(Held{"maps-German\\CoreCreek.OTA", text});
    MapTdfFile* holder = map_tdf_file_create(FileReader{&files, read_file, rescan});
    map_tdf_file_set_language(holder, "German");
    screens::PlanetInfo info{};
    screens::GalaxyPlanet planet =
        planet_named("CoreCreek.ota", screens::MapSource::core_contingency);
    fill(&info, &planet, holder);
    CHECK(files.asked.size() == 1 && files.asked[0] == "maps-German\\CoreCreek.OTA");
    CHECK(files.rescans == 0);
    CHECK(info.map_installed && planet.environment.gravity == 112);
    CHECK(planet.environment.min_wind == 100 && planet.environment.max_wind == 4000);
    CHECK(planet.environment.tidal_strength == 25 && !planet.environment.lavaworld);

    // The language file is missing, so the plain maps folder is the one read.
    files.asked.clear();
    files.present.clear();
    files.present.push_back(Held{"maps\\CoreCreek.OTA", text});
    planet.environment = {};
    fill(&info, &planet, holder);
    CHECK(files.asked.size() == 2);
    CHECK(files.asked[0] == "maps-German\\CoreCreek.OTA");
    CHECK(files.asked[1] == "maps\\CoreCreek.OTA");
    CHECK(files.rescans == 0 && info.map_installed);

    // No language named: only the plain folder, and the name's extension is dropped.
    map_tdf_file_set_language(holder, nullptr);
    files.asked.clear();
    planet.environment = {};
    planet = planet_named("CoreCreek.ota", screens::MapSource::ta_disk);
    fill(&info, &planet, holder);
    CHECK(files.asked.size() == 1 && files.asked[0] == "maps\\CoreCreek.OTA");
    CHECK(info.map_installed && files.rescans == 0);
    map_tdf_file_destroy(holder);
}

void test_base_disc_retry() {
    const std::vector<uint8_t> text = as_bytes(kMapText);
    Files files;
    files.after_rescan.push_back(Held{"maps\\CoreCreek.OTA", text});
    MapTdfFile* holder = map_tdf_file_create(FileReader{&files, read_file, rescan});
    screens::PlanetInfo info{};
    screens::GalaxyPlanet planet = planet_named("CoreCreek", screens::MapSource::ta_disk);
    fill(&info, &planet, holder);
    CHECK(files.rescans == 1);
    CHECK(files.asked.size() == 2);
    CHECK(files.asked[0] == "maps\\CoreCreek.OTA" && files.asked[1] == "maps\\CoreCreek.OTA");
    CHECK(info.map_installed && planet.environment.gravity == 112);
    CHECK(planet.environment.tidal_strength == 25);

    // A map that opens the first time is not scanned for again.
    files.asked.clear();
    files.rescans = 0;
    planet.environment = {};
    fill(&info, &planet, holder);
    CHECK(files.rescans == 0 && files.asked.size() == 1 && info.map_installed);

    // Already loaded: no further read.
    files.asked.clear();
    fill(&info, &planet, holder);
    CHECK(files.asked.empty() && files.rescans == 0);

    // Another source that fails is not retried, even though a scan would find it.
    files.present.clear();
    files.asked.clear();
    files.rescans = 0;
    planet = planet_named("CoreCreek", screens::MapSource::core_contingency);
    fill(&info, &planet, holder);
    CHECK(!info.map_installed && !planet.environment.loaded);
    CHECK(files.rescans == 0 && files.asked.size() == 1);

    // A language file that shows up only in the scan is not the path retried.
    map_tdf_file_set_language(holder, "German");
    files.present.clear();
    files.after_rescan.clear();
    files.after_rescan.push_back(Held{"maps-German\\CoreCreek.OTA", text});
    files.asked.clear();
    files.rescans = 0;
    planet = planet_named("CoreCreek", screens::MapSource::ta_disk);
    fill(&info, &planet, holder);
    CHECK(!info.map_installed);
    CHECK(files.rescans == 1 && files.asked.size() == 3);
    CHECK(files.asked[0] == "maps-German\\CoreCreek.OTA");
    CHECK(files.asked[1] == "maps\\CoreCreek.OTA");
    CHECK(files.asked[2] == "maps\\CoreCreek.OTA");

    // A file with no GlobalHeader is not loaded, and is not scanned for again.
    files.present.clear();
    files.present.push_back(Held{"maps-German\\CoreCreek.OTA", as_bytes(kNoHeader)});
    files.asked.clear();
    files.rescans = 0;
    planet.environment = {};
    fill(&info, &planet, holder);
    CHECK(!info.map_installed && files.rescans == 0 && files.asked.size() == 1);
    map_tdf_file_destroy(holder);
}

} // namespace

int main() {
    test_language_folder();
    test_base_disc_retry();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("map loading tests passed\n");
    return 0;
}
