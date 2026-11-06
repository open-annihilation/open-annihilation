// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The galaxy load of the Boneyards system object: the war-map feeds are
// requested, the war-map file is read through the service's sector-map
// requests and the galaxy view is built from its star layers, planets and
// connectors, each planet with its Planets.gaf sequences and name label.
#include "galaxy_graphics.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace oa::boneyards::console {

namespace {

using PlanetSequences = std::array<const formats::gaf::Sequence*, k_planet_sprite_sizes>;

constexpr std::array<const char*, k_planet_sprite_sizes> k_sprite_sizes = {"25", "50", "75", "100"};
constexpr int32_t k_size_class_count = 3;
constexpr std::array<const char*, k_size_class_count> k_size_classes = {"_L", "_M", "_S"};
constexpr std::size_t k_sequence_name_capacity = 0x40;
// The sprite rectangle spans the frames of the largest size.
constexpr std::size_t k_bounds_size = k_planet_sprite_sizes - 1;

// The planet's sequence for each size: its base name, the size and the size
// class suffix, looked up in Planets.gaf. False for an unknown size class or
// a missing sequence.
bool resolve_sequences(
    const std::optional<formats::gaf::Archive>& archive,
    const SectorPlanetRecord& planet,
    PlanetSequences& out
) {
    if (planet.size_class < 0 || planet.size_class >= k_size_class_count)
        return false;
    for (std::size_t size = 0; size < k_planet_sprite_sizes; ++size) {
        char name[k_sequence_name_capacity];
        std::snprintf(
            name,
            sizeof name,
            "%.*s%s%s",
            static_cast<int>(sizeof planet.seqname),
            planet.seqname,
            k_sprite_sizes[size],
            k_size_classes[static_cast<std::size_t>(planet.size_class)]
        );
        out[size] = archive ? galaxy_find_sequence(*archive, name) : nullptr;
        if (out[size] == nullptr)
            return false;
    }
    return true;
}

screens::PlanetRecord planet_record(const SectorPlanetRecord& p) {
    screens::PlanetRecord r{};
    r.id = p.id;
    r.name = p.name;
    r.x = p.x;
    r.y = p.y;
    r.sequence = p.seqname;
    r.variant_group = p.size_class;
    r.frame_interval = p.planet_rot;
    r.side = p.side;
    r.capitol = p.capitol;
    r.description = p.desc;
    r.ta_map = p.ta_map;
    r.ta_mapsource = p.ta_mapsource;
    r.ta_maptype = p.ta_maptype;
    r.pan_gaf = p.pan_gaf;
    r.pan_seq = p.pan_seq;
    const int32_t settings[screens::kPlanetSettingCount] = {
        p.playerlimit,
        p.deathopt,
        p.energy,
        p.metal,
        p.location,
        p.los,
        p.mapping,
        p.maxunits,
        p.watching,
    };
    std::memcpy(r.settings, settings, sizeof r.settings);
    return r;
}

// The frame count is the smallest size's and the rectangle grows over as
// many frames of the largest size (the installed sizes of a planet have equal counts).
void append_sprite_frames(
    const PlanetSequences& sequences, std::vector<screens::SpriteFrame>& frames, std::size_t* count
) {
    const std::size_t frame_count = sequences[0]->frames.size();
    const std::vector<formats::gaf::Frame>& largest = sequences[k_bounds_size]->frames;
    *count = frame_count < largest.size() ? frame_count : largest.size();
    for (std::size_t i = 0; i < *count; ++i)
        frames.push_back(
            screens::SpriteFrame{
                largest[i].width, largest[i].height, largest[i].origin_x, largest[i].origin_y
            }
        );
}

// Everything between opening the war-map file and closing it.
bool read_galaxy(
    screens::GalaxyView* view,
    GalaxyGraphics& graphics,
    const BoneyardsService& service,
    SectorMapReader* reader,
    const SectorMapHeader& header
) {
    if (header.width < screens::kGalaxyMapWidth || header.height < screens::kGalaxyMapHeight)
        return false;
    view->max_scroll_x = header.width - view->view_width;
    view->max_scroll_y = header.height - view->view_height;
    if (header.planet_count < 0 ||
        static_cast<std::size_t>(header.planet_count) > screens::kMaxGalaxyPlanets ||
        header.connector_count < 0 ||
        static_cast<std::size_t>(header.connector_count) > screens::kMaxGalaxyConnectors)
        return false;
    const auto planet_count = static_cast<std::size_t>(header.planet_count);
    const auto connector_count = static_cast<std::size_t>(header.connector_count);
    const auto planets = std::make_unique<SectorPlanetRecord[]>(planet_count);
    const auto connectors = std::make_unique<ConnectorRecord[]>(connector_count);
    if (!service.read_sector_map(
            service.user,
            reader,
            header.width,
            header.height,
            planets.get(),
            header.planet_count,
            connectors.get(),
            header.connector_count
        ))
        return false;

    std::array<screens::PlanetRecord, screens::kMaxGalaxyPlanets> records{};
    std::array<PlanetSequences, screens::kMaxGalaxyPlanets> sequences{};
    std::array<std::size_t, screens::kMaxGalaxyPlanets> frame_counts{};
    std::vector<screens::SpriteFrame> frames;
    for (std::size_t i = 0; i < planet_count; ++i) {
        if (!resolve_sequences(graphics.planets, planets[i], sequences[i]))
            return false;
        records[i] = planet_record(planets[i]);
        append_sprite_frames(sequences[i], frames, &frame_counts[i]);
    }
    const screens::PlanetSprites sprites{frames.data(), frame_counts.data()};
    if (!screens::galaxy_load(
            view,
            header.width,
            header.height,
            records.data(),
            planet_count,
            connectors.get(),
            connector_count,
            &sprites
        ))
        return false;
    graphics.planet_sequences = {};
    for (std::size_t at = 0; at < view->planet_count; ++at) {
        for (std::size_t i = 0; i < planet_count; ++i)
            if (records[i].id == view->planets[at].id)
                graphics.planet_sequences[at] = sequences[i];
        galaxy_render_label(graphics, at, view->planets[at].name);
    }
    return true;
}

} // namespace

bool galaxy_view_load(
    screens::GalaxyView* view,
    GalaxyGraphics& graphics,
    const BoneyardsService& service,
    const char* path
) {
    service.request_allegiance(service.user);
    service.request_census(service.user, 0);
    screens::galaxy_clear(view);
    galaxy_release_panoramas(graphics);
    SectorMapHeader header{};
    SectorMapReader* reader = service.open_sector_map(service.user, path, &header);
    if (reader == nullptr)
        return false;
    view->sector_id = header.sector_id;
    std::memcpy(view->sector_name, header.sector_name, sizeof view->sector_name);
    view->sector_name[sizeof view->sector_name - 1] = '\0';
    view->map_width = header.width;
    view->map_height = header.height;
    const bool loaded = read_galaxy(view, graphics, service, reader, header);
    service.close_sector_map(service.user, reader);
    return loaded;
}

} // namespace oa::boneyards::console
