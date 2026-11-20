// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Galactic War sector map slots (34-36) over synthetic maps: the header
// checks, every planet range check and default, the truncating copies, and
// connector validation. With --data, the shipped BYPacific_map.gwm of the
// BY1117a install OA_GAME_DIR names instead.

#include "oa/boneyards_test/installs.hpp"
#include "oa/rover/interface.h"
#include "oa/platform/files.hpp"

#include <stdio.h>
#include <string.h>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

const RoverInterface* rover = nullptr;
const char* const kPath = "rover-sector-map-test.gwm";

constexpr const char* kHeader =
    "{[Map<S:command=sector><I:id=45><S:name=New Hub><I:width=2000><I:height=1400>"
    "<I:planets=2><I:connectors=1>]}\r\n";
constexpr const char* kPlanetA =
    "{[Map<S:command=planet><I:id=1803><S:name=Fuel Dump><I:x=991><I:y=849><S:seqname=Metal_L>"
    "<S:side=A><I:capitol=0><S:desc=A metal world%8><S:ta_map=Over Crude Water>"
    "<S:ta_mapsource=TA><S:ta_maptype=Metal><S:pan_gaf=Metalbrief.gaf><I:planet_rot=5>"
    "<S:pan_seq=MetalPan><I:playerlimit=4><I:watching=2><I:los=3><I:location=1><I:deathopt=2>"
    "<I:maxunits=500><I:metal=10000><I:energy=0><I:mapping=1>]}\r\n";
constexpr const char* kPlanetB =
    "{[Map<S:command=planet><I:id=1804><S:name=Respite><I:x=20><I:y=1355><S:seqname=Green_m>"
    "<S:side=C><I:capitol=1><S:desc=Forest><S:ta_map=Sherwood><S:ta_mapsource=CC>"
    "<S:ta_maptype=Green><S:pan_gaf=Greenbrief.gaf><S:pan_seq=GreenPan>]}\r\n";
constexpr const char* kConnector =
    "{[Map<S:command=connector><I:planet1=1803><I:planet2=1804>]}\r\n";

struct Header {
    int32_t id = -2;
    char name[0x40];
    int32_t width = -2;
    int32_t height = -2;
    int32_t planets = -2;
    int32_t connectors = -2;
};

void write_map(
    const char* header, const char* planet_a, const char* planet_b, const char* connector
) {
    FILE* file = oa::platform::open_file(kPath, "wb");
    fputs(header, file);
    fputs(planet_a, file);
    fputs(planet_b, file);
    fputs(connector, file);
    fclose(file);
}

RoverSectorReader* open_map(Header* header) {
    memset(header->name, '#', sizeof header->name);
    return rover->open_sector_map(
        kPath,
        &header->id,
        header->name,
        &header->width,
        &header->height,
        &header->planets,
        &header->connectors
    );
}

// Opens the map and reads its planets; the result of slot 35, or false when
// the header already fails.
bool read_map(
    RoverSectorPlanet* planets, RoverSectorConnector* connectors, int32_t connector_count = 1
) {
    Header header{};
    RoverSectorReader* reader = open_map(&header);
    if (!reader)
        return false;
    bool read = rover->read_sector_planets(
        reader, header.width, header.height, planets, header.planets, connectors, connector_count
    );
    rover->close_sector_map(reader);
    return read;
}

// A copy of text with the first occurrence of from replaced by to.
const char* edited(const char* text, const char* from, const char* to) {
    static char buffer[4096];
    const char* at = strstr(text, from);
    size_t head = static_cast<size_t>(at - text);
    snprintf(
        buffer, sizeof buffer, "%.*s%s%s", static_cast<int>(head), text, to, at + strlen(from)
    );
    return buffer;
}

void check_header() {
    write_map(kHeader, kPlanetA, kPlanetB, kConnector);
    Header header{};
    RoverSectorReader* reader = open_map(&header);
    check(reader != nullptr, "open_sector_map");
    check(
        header.id == 45 && strcmp(header.name, "New Hub") == 0 && header.width == 2000 &&
            header.height == 1400 && header.planets == 2 && header.connectors == 1,
        "header fields"
    );
    check(header.name[8] == '#', "the name copy writes only the text and its terminator");
    rover->close_sector_map(reader);
    rover->close_sector_map(nullptr);

    struct Broken {
        const char* from;
        const char* to;
        const char* what;
    };

    const Broken broken[] = {
        {"[Map<", "[Mapx<", "another entry name"},
        {"command=sector", "command=planet", "another command"},
        {"<I:connectors=1>", "", "a missing count"},
        {"<I:id=45>", "<I:id=-1>", "an id of -1"},
        {"<I:width=2000>", "<S:width=2000>", "a width that is not type I"},
        {"<S:name=New Hub>", "<S:name=>", "an empty sector name is refused"},
        {"<S:name=New Hub>",
         "<S:name=123456789012345678901234567890123456789012345678901>",
         "a 51-character name"},
        {"<I:id=45>", "<I:id=45><I:id=46>", "a repeated key"},
        {"{[Map", "{<S:a=1>][Map", "a first entry without a name"},
    };
    for (const Broken& b : broken) {
        write_map(edited(kHeader, b.from, b.to), kPlanetA, kPlanetB, kConnector);
        reader = open_map(&header);
        check(reader == nullptr, b.what);
        rover->close_sector_map(reader);
    }
    write_map(
        edited(
            kHeader,
            "<S:name=New Hub>",
            "<S:name=12345678901234567890123456789012345678901234567890>"
        ),
        "",
        "",
        ""
    );
    reader = open_map(&header);
    check(reader != nullptr && strlen(header.name) == 50, "a 50-character name");
    rover->close_sector_map(reader);

    remove(kPath);
    check(open_map(&header) == nullptr, "a missing file");
}

void check_planets() {
    write_map(kHeader, kPlanetA, kPlanetB, kConnector);
    RoverSectorPlanet planets[2];
    memset(planets, 0x5a, sizeof planets);
    RoverSectorConnector connectors[1]{};
    check(read_map(planets, connectors), "read_sector_planets");

    const RoverSectorPlanet& a = planets[0];
    check(
        a.id == 1803 && strcmp(a.name, "Fuel Dump") == 0 && a.x == 991 && a.y == 849,
        "planet identity"
    );
    check(strcmp(a.seqname, "Metal_") == 0 && a.size_class == 0, "seqname loses its size letter");
    check(
        a.side == 'A' && !a.capitol && strcmp(a.desc, "A metal world%") == 0, "side, capitol, desc"
    );
    check(
        strcmp(a.ta_map, "Over Crude Water") == 0 && strcmp(a.ta_mapsource, "TA") == 0 &&
            strcmp(a.ta_maptype, "Metal") == 0 && strcmp(a.pan_gaf, "Metalbrief.gaf") == 0 &&
            strcmp(a.pan_seq, "MetalPan") == 0,
        "map and panel names"
    );
    check(
        a.planet_rot == 5 && a.playerlimit == 4 && a.deathopt == 2 && a.energy == 0 &&
            a.metal == 10000 && a.location == 1 && a.los == 3 && a.mapping == 1 &&
            a.maxunits == 500 && a.watching == 2,
        "optional integers as given"
    );
    check(a.name[10] == 0x5a, "text copies leave the rest of the field");

    const RoverSectorPlanet& b = planets[1];
    check(b.size_class == 1 && strcmp(b.seqname, "Green_") == 0, "a lower-case size letter");
    check(b.side == 'C' && b.capitol && b.x == 20 && b.y == 1355, "edge coordinates are inside");
    check(
        b.planet_rot == 3 && b.playerlimit == 2 && b.deathopt == 0 && b.energy == 1000 &&
            b.metal == 1000 && b.location == 2 && b.los == 1 && b.mapping == 2 &&
            b.maxunits == 200 && b.watching == 1,
        "optional integers default"
    );
    check(connectors[0].planet1_id == 1803 && connectors[0].planet2_id == 1804, "the connector");

    // Accepted lengths above what the destination keeps are cut short.
    char long_map[80];
    memset(long_map, 'm', 63);
    long_map[63] = 0;
    char replacement[100];
    snprintf(replacement, sizeof replacement, "<S:ta_map=%s>", long_map);
    const char* planet = edited(kPlanetB, "<S:ta_map=Sherwood>", replacement);
    char planet_b[4096];
    snprintf(planet_b, sizeof planet_b, "%s", planet);
    planet = edited(planet_b, "<S:ta_mapsource=CC>", "<S:ta_mapsource=ABCD>");
    snprintf(planet_b, sizeof planet_b, "%s", planet);
    planet = edited(planet_b, "<S:ta_maptype=Green>", "<S:ta_maptype=0123456789abcdef>");
    write_map(kHeader, kPlanetA, planet, kConnector);
    check(
        read_map(planets, connectors) && strlen(planets[1].ta_map) == 62 &&
            strcmp(planets[1].ta_mapsource, "ABC") == 0 &&
            strcmp(planets[1].ta_maptype, "0123456789abcde") == 0,
        "ta_map, ta_mapsource and ta_maptype keep one character less than they accept"
    );
}

void check_planet_rejections() {
    struct Broken {
        const char* from;
        const char* to;
        const char* what;
    };

    const Broken broken[] = {
        {"[Map<", "[Planet<", "another entry name"},
        {"command=planet", "command=sector", "another command"},
        {"<S:pan_seq=GreenPan>", "", "a missing required field"},
        {"<I:id=1804>", "<S:id=1804>", "an id that is not type I"},
        {"<I:id=1804>", "<I:id=1803>", "a repeated id"},
        {"<S:name=Respite>", "<S:name=Fuel Dump>", "a repeated name"},
        {"<S:name=Respite>", "<S:name=Respite Respite1>", "a 16-character name"},
        {"<S:seqname=Green_m>", "<S:seqname=Green_X>", "an unknown size letter"},
        {"<S:seqname=Green_m>", "<S:seqname=GreenM>", "no '_' before the size letter"},
        {"<S:seqname=Green_m>", "<S:seqname=M>", "a one-character seqname"},
        {"<S:side=C>", "<S:side=X>", "an unknown side"},
        {"<S:side=C>", "<S:side=CC>", "a two-character side"},
        {"<I:capitol=1>", "<I:capitol=2>", "a capitol of 2"},
        {"<I:x=20>", "<I:x=19>", "x left of the margin"},
        {"<I:x=20>", "<I:x=1941>", "x right of width - 60"},
        {"<I:y=1355>", "<I:y=19>", "y above the margin"},
        {"<I:y=1355>", "<I:y=1356>", "y below height - 45"},
        {"<S:ta_mapsource=CC>", "<S:ta_mapsource=ABCDE>", "a 5-character ta_mapsource"},
        {"<S:pan_seq=GreenPan>", "<S:pan_seq=GreenPan><I:playerlimit=5>", "a playerlimit of 5"},
        {"<S:pan_seq=GreenPan>", "<S:pan_seq=GreenPan><I:playerlimit=1>", "a playerlimit of 1"},
        {"<S:pan_seq=GreenPan>",
         "<S:pan_seq=GreenPan><S:playerlimit=3>",
         "an optional field of type S"},
        {"<S:pan_seq=GreenPan>", "<S:pan_seq=GreenPan><I:planet_rot=-1>", "a negative planet_rot"},
        {"<S:pan_seq=GreenPan>", "<S:pan_seq=GreenPan><I:energy=10001>", "energy above 10000"},
        {"<S:pan_seq=GreenPan>", "<S:pan_seq=GreenPan><I:location=3>", "a location of 3"},
        {"<S:pan_seq=GreenPan>", "<S:pan_seq=GreenPan><I:los=4>", "a los of 4"},
        {"<S:pan_seq=GreenPan>", "<S:pan_seq=GreenPan><I:maxunits=0>", "a maxunits of 0"},
        {"<S:pan_seq=GreenPan>", "<S:pan_seq=GreenPan><I:watching=0>", "a watching of 0"},
    };
    RoverSectorPlanet planets[2];
    RoverSectorConnector connectors[1];
    for (const Broken& b : broken) {
        write_map(kHeader, kPlanetA, edited(kPlanetB, b.from, b.to), kConnector);
        memset(planets, 0x5a, sizeof planets);
        check(!read_map(planets, connectors), b.what);
        check(
            planets[0].id == 1803 && planets[1].id == 0x5a5a5a5a, "a failed planet writes nothing"
        );
    }

    const Broken connector_broken[] = {
        {"planet2=1804", "planet2=1805", "a connector to an unknown planet"},
        {"<I:planet1=1803>", "", "a connector missing an id"},
        {"command=connector", "command=planet", "a connector with another command"},
    };
    for (const Broken& b : connector_broken) {
        write_map(kHeader, kPlanetA, kPlanetB, edited(kConnector, b.from, b.to));
        check(!read_map(planets, connectors), b.what);
    }
    write_map(kHeader, kPlanetA, kPlanetB, kConnector);
    check(!read_map(planets, connectors, 2), "more connectors than records");
    check(!read_map(planets, connectors, -1), "a negative connector count");
    check(read_map(planets, connectors, 0), "no connectors asked for");
}

/// Checks the installed BYPacific_map.gwm's header, its planets and connectors, and its first planet.
///
/// @param game_dir the BY1117a install holding the Boneyards files
void check_shipped_map(const char* game_dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/Boneyards/BYPacific_map.gwm", game_dir);
    Header header{};
    RoverSectorReader* reader = rover->open_sector_map(
        path,
        &header.id,
        header.name,
        &header.width,
        &header.height,
        &header.planets,
        &header.connectors
    );
    check(reader != nullptr, "the shipped BYPacific_map.gwm opens");
    if (!reader)
        return;
    check(
        header.id == 45 && strcmp(header.name, "New Hub") == 0 && header.width == 2000 &&
            header.height == 1400 && header.planets == 25 && header.connectors == 32,
        "the shipped sector header"
    );
    static RoverSectorPlanet planets[25];
    static RoverSectorConnector connectors[32];
    check(
        rover->read_sector_planets(
            reader, header.width, header.height, planets, 25, connectors, 32
        ),
        "every shipped planet and connector validates"
    );
    check(
        planets[0].id == 1803 && strcmp(planets[0].name, "Fuel Dump") == 0 &&
            strcmp(planets[0].seqname, "Metal_") == 0 && planets[0].size_class == 0,
        "the first shipped planet"
    );
    rover->close_sector_map(reader);
}

} // namespace

int main(int argc, char** argv) {
    rover_get_interface(&rover);
    if (oa::test::game_data_requested(argc, argv)) {
        const auto game_dir = oa::boneyards_test::require_by_install(
            "the shipped Boneyards sector map", {"Boneyards/BYPacific_map.gwm"}
        );
        check_shipped_map(game_dir.string().c_str());
    } else {
        // Only these cases write the scratch map; the --data run, which ctest
        // may start beside them in the same folder, leaves it alone.
        check_header();
        check_planets();
        check_planet_rejections();
        remove(kPath);
    }
    if (failures)
        return 1;
    puts("rover sector map: ok");
    return 0;
}
