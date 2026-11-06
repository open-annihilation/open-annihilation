// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Galactic War sector map reader behind slots 34-36: a "sector" header
// record, then the planet and connector records the header counts, each a
// "Map" entry read with the record reader and range-checked field by field.

#include "heap.h"
#include "records.h"
#include "slots.h"
#include "text.h"

#include "oa/rover/strings.h"

#include <new>

struct RoverSectorReader {
    RoverRecordReader reader{};
};

namespace {

constexpr const char* kMapEntryName = "Map";
// Integer fields read -1 when absent or not type I, which every check rejects.
constexpr int32_t kMissingInt = -1;

// Header limits.
constexpr uint32_t kSectorNameMaxLength = 0x32;

// Planet positions keep this far inside the sector's width and height.
constexpr int32_t kPlanetMinCoordinate = 0x14;
constexpr int32_t kPlanetRightMargin = 0x3c;
constexpr int32_t kPlanetBottomMargin = 0x2d;

// Longest accepted text of each planet field; some copies keep less.
constexpr uint32_t kPlanetNameMaxLength = 0xf;
constexpr uint32_t kSeqnameMaxLength = 0x1f;
constexpr uint32_t kDescMaxLength = 0x3ff;
constexpr uint32_t kTaMapMaxLength = 0x3f;
constexpr uint32_t kTaMapsourceMaxLength = 4;
constexpr uint32_t kTaMaptypeMaxLength = 0x10;
constexpr uint32_t kPanelMaxLength = 0x1f;

struct OptionalInt {
    const char* key;
    int32_t fallback;
};

// Optional planet integers and the values they take when absent.
constexpr OptionalInt kPlanetRot{"planet_rot", 3};
constexpr OptionalInt kPlayerLimit{"playerlimit", 2};
constexpr OptionalInt kDeathOpt{"deathopt", 0};
constexpr OptionalInt kEnergy{"energy", 1000};
constexpr OptionalInt kMetal{"metal", 1000};
constexpr OptionalInt kLocation{"location", 2};
constexpr OptionalInt kLos{"los", 1};
constexpr OptionalInt kMapping{"mapping", 2};
constexpr OptionalInt kMaxUnits{"maxunits", 200};
constexpr OptionalInt kWatching{"watching", 1};

constexpr int32_t kMaxResources = 10000;
constexpr int32_t kMaxUnitLimit = 500;

bool in_range(int32_t value, int32_t low, int32_t high) {
    return value >= low && value <= high;
}

bool one_or_two(int32_t value) {
    return value == 1 || value == 2;
}

bool text_length_in(const RoverString* text, uint32_t max_length) {
    return text->length != 0 && text->length <= max_length;
}

int32_t optional_int(RoverRecordEntry* entry, const OptionalInt& field) {
    return rover_record_entry_int(entry, field.key, field.fallback);
}

// Size class from a seqname's "_L", "_M" or "_S" ending, or -1.
int32_t seqname_size_class(const RoverString* seqname) {
    if (seqname->length < 2 || seqname->data[seqname->length - 2] != '_')
        return -1;
    switch (seqname->data[seqname->length - 1]) {
    case 'L':
    case 'l':
        return 0;
    case 'M':
    case 'm':
        return 1;
    case 'S':
    case 's':
        return 2;
    default:
        return -1;
    }
}

// The fields every planet record must carry.
struct PlanetFields {
    const RoverRecordField* command;
    const RoverRecordField* id;
    const RoverRecordField* name;
    const RoverRecordField* x;
    const RoverRecordField* y;
    const RoverRecordField* seqname;
    const RoverRecordField* side;
    const RoverRecordField* capitol;
    const RoverRecordField* desc;
    const RoverRecordField* ta_map;
    const RoverRecordField* ta_mapsource;
    const RoverRecordField* ta_maptype;
    const RoverRecordField* pan_gaf;
    const RoverRecordField* pan_seq;
};

bool find_planet_fields(RoverRecordEntry* entry, PlanetFields* fields) {
    *fields = PlanetFields{
        rover_record_entry_find(entry, "command"),
        rover_record_entry_find(entry, "id"),
        rover_record_entry_find(entry, "name"),
        rover_record_entry_find(entry, "x"),
        rover_record_entry_find(entry, "y"),
        rover_record_entry_find(entry, "seqname"),
        rover_record_entry_find(entry, "side"),
        rover_record_entry_find(entry, "capitol"),
        rover_record_entry_find(entry, "desc"),
        rover_record_entry_find(entry, "ta_map"),
        rover_record_entry_find(entry, "ta_mapsource"),
        rover_record_entry_find(entry, "ta_maptype"),
        rover_record_entry_find(entry, "pan_gaf"),
        rover_record_entry_find(entry, "pan_seq"),
    };
    return fields->command && fields->id && fields->name && fields->x && fields->y &&
           fields->seqname && fields->side && fields->capitol && fields->desc && fields->ta_map &&
           fields->ta_mapsource && fields->ta_maptype && fields->pan_gaf && fields->pan_seq;
}

// Validates one planet record against the sector size and the planets read
// before it, then fills planet; nothing is written when a check fails.
bool read_planet(
    RoverRecordEntry* entry,
    int32_t sector_width,
    int32_t sector_height,
    const RoverSectorPlanet* earlier,
    int32_t earlier_count,
    RoverSectorPlanet* planet
) {
    PlanetFields fields{};
    if (!rover_record_entry_is(entry, kMapEntryName) || !find_planet_fields(entry, &fields) ||
        !rover_string_equals(&fields.command->value, "planet"))
        return false;
    int32_t id = rover_record_field_int(fields.id, kMissingInt);
    const RoverString* name = &fields.name->value;
    int32_t x = rover_record_field_int(fields.x, kMissingInt);
    int32_t y = rover_record_field_int(fields.y, kMissingInt);
    const RoverString* seqname = &fields.seqname->value;
    const RoverString* side = &fields.side->value;
    int32_t capitol = rover_record_field_int(fields.capitol, kMissingInt);
    int32_t planet_rot = optional_int(entry, kPlanetRot);
    int32_t playerlimit = optional_int(entry, kPlayerLimit);
    int32_t deathopt = optional_int(entry, kDeathOpt);
    int32_t energy = optional_int(entry, kEnergy);
    int32_t metal = optional_int(entry, kMetal);
    int32_t location = optional_int(entry, kLocation);
    int32_t los = optional_int(entry, kLos);
    int32_t mapping = optional_int(entry, kMapping);
    int32_t maxunits = optional_int(entry, kMaxUnits);
    int32_t watching = optional_int(entry, kWatching);

    if (id == kMissingInt || !text_length_in(name, kPlanetNameMaxLength) ||
        !text_length_in(seqname, kSeqnameMaxLength) || side->length != 1 ||
        !(rover_string_equals(side, "A") || rover_string_equals(side, "C") ||
          rover_string_equals(side, "I")) ||
        (capitol != 1 && capitol != 0) || !text_length_in(&fields.desc->value, kDescMaxLength) ||
        !text_length_in(&fields.ta_map->value, kTaMapMaxLength) ||
        !text_length_in(&fields.ta_mapsource->value, kTaMapsourceMaxLength) ||
        !text_length_in(&fields.ta_maptype->value, kTaMaptypeMaxLength) ||
        !text_length_in(&fields.pan_gaf->value, kPanelMaxLength) ||
        !text_length_in(&fields.pan_seq->value, kPanelMaxLength))
        return false;
    if (planet_rot < 0 || !in_range(playerlimit, 2, 4) || !in_range(deathopt, 0, 2) ||
        !in_range(energy, 0, kMaxResources) || !in_range(metal, 0, kMaxResources) ||
        !one_or_two(location) || !in_range(los, 1, 3) || !one_or_two(mapping) ||
        !in_range(maxunits, 1, kMaxUnitLimit) || !one_or_two(watching))
        return false;
    if (!in_range(x, kPlanetMinCoordinate, sector_width - kPlanetRightMargin) ||
        !in_range(y, kPlanetMinCoordinate, sector_height - kPlanetBottomMargin))
        return false;
    for (int32_t i = 0; i < earlier_count; ++i)
        if (earlier[i].id == id || rover_string_equals(name, earlier[i].name))
            return false;
    int32_t size_class = seqname_size_class(seqname);
    if (size_class < 0)
        return false;

    planet->id = id;
    rover_copy_text(planet->name, rover_string_text(name), sizeof planet->name);
    planet->x = x;
    planet->y = y;
    // Bounded by the length itself, which drops the size letter.
    rover_copy_text(
        planet->seqname, rover_string_text(seqname), static_cast<int32_t>(seqname->length)
    );
    planet->size_class = size_class;
    planet->side = rover_string_text(side)[0];
    planet->capitol = capitol == 1;
    rover_copy_text(planet->desc, rover_string_text(&fields.desc->value), sizeof planet->desc);
    rover_copy_text(
        planet->ta_map, rover_string_text(&fields.ta_map->value), sizeof planet->ta_map
    );
    rover_copy_text(
        planet->ta_mapsource,
        rover_string_text(&fields.ta_mapsource->value),
        sizeof planet->ta_mapsource
    );
    rover_copy_text(
        planet->ta_maptype, rover_string_text(&fields.ta_maptype->value), sizeof planet->ta_maptype
    );
    rover_copy_text(
        planet->pan_gaf, rover_string_text(&fields.pan_gaf->value), sizeof planet->pan_gaf
    );
    rover_copy_text(
        planet->pan_seq, rover_string_text(&fields.pan_seq->value), sizeof planet->pan_seq
    );
    planet->planet_rot = planet_rot;
    planet->playerlimit = playerlimit;
    planet->deathopt = deathopt;
    planet->energy = energy;
    planet->metal = metal;
    planet->location = location;
    planet->los = los;
    planet->mapping = mapping;
    planet->maxunits = maxunits;
    planet->watching = watching;
    return true;
}

// Reads one connector record whose two ids name planets already read.
bool read_connector(
    RoverRecordEntry* entry,
    const RoverSectorPlanet* planets,
    int32_t planet_count,
    RoverSectorConnector* connector
) {
    if (!rover_record_entry_is(entry, kMapEntryName))
        return false;
    const RoverRecordField* command = rover_record_entry_find(entry, "command");
    const RoverRecordField* planet1 = rover_record_entry_find(entry, "planet1");
    const RoverRecordField* planet2 = rover_record_entry_find(entry, "planet2");
    if (!command || !planet1 || !planet2 || !rover_string_equals(&command->value, "connector"))
        return false;
    int32_t first = rover_record_field_int(planet1, kMissingInt);
    int32_t second = rover_record_field_int(planet2, kMissingInt);
    bool first_known = false;
    bool second_known = false;
    for (int32_t i = 0; i < planet_count; ++i) {
        first_known = first_known || planets[i].id == first;
        second_known = second_known || planets[i].id == second;
    }
    if (!first_known || !second_known)
        return false;
    connector->planet1_id = first;
    connector->planet2_id = second;
    return true;
}

} // namespace

RoverSectorReader* ROVER_CALL rover_open_sector_map(
    const char* path,
    int32_t* out_sector_id,
    char* out_sector_name,
    int32_t* out_width,
    int32_t* out_height,
    int32_t* out_planet_count,
    int32_t* out_connector_count
) {
    void* block = rover_heap_alloc(sizeof(RoverSectorReader));
    if (!block)
        return nullptr;
    RoverSectorReader* map = new (block) RoverSectorReader{};
    rover_record_reader_init(&map->reader, path);
    RoverFileRecord* record = nullptr;
    if (rover_record_reader_open(&map->reader) != 0 ||
        rover_record_reader_next(&map->reader, &record) != RoverReadStatus::ok) {
        rover_close_sector_map(map);
        return nullptr;
    }
    RoverRecordEntry* entry = rover_record_first_entry(record);
    const RoverRecordField* command = nullptr;
    const RoverRecordField* id = nullptr;
    const RoverRecordField* name = nullptr;
    const RoverRecordField* width = nullptr;
    const RoverRecordField* height = nullptr;
    const RoverRecordField* planets = nullptr;
    const RoverRecordField* connectors = nullptr;
    if (rover_record_entry_is(entry, kMapEntryName)) {
        command = rover_record_entry_find(entry, "command");
        id = rover_record_entry_find(entry, "id");
        name = rover_record_entry_find(entry, "name");
        width = rover_record_entry_find(entry, "width");
        height = rover_record_entry_find(entry, "height");
        planets = rover_record_entry_find(entry, "planets");
        connectors = rover_record_entry_find(entry, "connectors");
    }
    bool valid = command && id && name && width && height && planets && connectors &&
                 rover_string_equals(&command->value, "sector");
    if (valid) {
        int32_t sector_id = rover_record_field_int(id, kMissingInt);
        int32_t sector_width = rover_record_field_int(width, kMissingInt);
        int32_t sector_height = rover_record_field_int(height, kMissingInt);
        int32_t planet_count = rover_record_field_int(planets, kMissingInt);
        int32_t connector_count = rover_record_field_int(connectors, kMissingInt);
        valid = sector_id != kMissingInt && text_length_in(&name->value, kSectorNameMaxLength) &&
                sector_width != kMissingInt && sector_height != kMissingInt &&
                planet_count != kMissingInt && connector_count != kMissingInt;
        if (valid) {
            *out_sector_id = sector_id;
            rover_copy_text(
                out_sector_name,
                rover_string_text(&name->value),
                static_cast<int32_t>(kSectorNameMaxLength + 1)
            );
            *out_width = sector_width;
            *out_height = sector_height;
            *out_planet_count = planet_count;
            *out_connector_count = connector_count;
        }
    }
    rover_file_record_destroy(record);
    if (!valid) {
        rover_close_sector_map(map);
        return nullptr;
    }
    return map;
}

bool ROVER_CALL rover_read_sector_planets(
    RoverSectorReader* reader,
    int32_t sector_width,
    int32_t sector_height,
    RoverSectorPlanet* out_planets,
    int32_t planet_count,
    RoverSectorConnector* out_connectors,
    int32_t connector_count
) {
    if (!reader || connector_count < 0)
        return false;
    for (int32_t i = 0; i < planet_count; ++i) {
        RoverFileRecord* record = nullptr;
        if (rover_record_reader_next(&reader->reader, &record) != RoverReadStatus::ok)
            return false;
        bool read = read_planet(
            rover_record_first_entry(record),
            sector_width,
            sector_height,
            out_planets,
            i,
            &out_planets[i]
        );
        rover_file_record_destroy(record);
        if (!read)
            return false;
    }
    for (int32_t i = 0; i < connector_count; ++i) {
        RoverFileRecord* record = nullptr;
        if (rover_record_reader_next(&reader->reader, &record) != RoverReadStatus::ok)
            return false;
        bool read = read_connector(
            rover_record_first_entry(record), out_planets, planet_count, &out_connectors[i]
        );
        rover_file_record_destroy(record);
        if (!read)
            return false;
    }
    return true;
}

void ROVER_CALL rover_close_sector_map(RoverSectorReader* reader) {
    if (!reader)
        return;
    rover_record_reader_close(&reader->reader);
    rover_heap_free(reader);
}
