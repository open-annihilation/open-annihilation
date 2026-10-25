// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "check.hpp"

#include "oa/data/persist/save_sections.hpp"
#include "oa/base/text.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace oa;
using namespace oa::data::persist;

namespace {

constexpr int32_t map_w = 4, map_h = 3;

// Reference handles: kind in the top byte, index below.
constexpr oa_ref32 ref(uint32_t kind, uint32_t index) {
    return (kind << 24) | (index + 1);
}

constexpr uint32_t ref_movement = 4, ref_info = 7, ref_rules = 8;

struct Fixture {
    std::unique_ptr<World> world_state = std::make_unique<World>();
    Game* game = &world_state->game;
    std::vector<uint8_t> plots = std::vector<uint8_t>(map_w * map_h * plot_bytes);
    std::vector<uint8_t> mapping = std::vector<uint8_t>(map_w * map_h / 2);
    std::vector<uint8_t> feature_records = std::vector<uint8_t>(8 * feature_record_bytes);
    std::vector<FeatureDef> feature_defs = std::vector<FeatureDef>(3);
    std::vector<Unit> units = std::vector<Unit>(4);
    std::vector<UnitDef> unit_defs = std::vector<UnitDef>(1);
    std::vector<uint8_t> movement = std::vector<uint8_t>(0x40);
    // Unit 1's queues.
    std::vector<SavedOrder> primary, secondary;
    PlayerSetupInfo info{};
    std::vector<uint8_t> rules = std::vector<uint8_t>(0x120);
    oa::sim::world_environment::MeteorState meteor{};
    SaveHooks hooks{};
    SaveContext world{};
    std::vector<std::string> log;

    Fixture() {
        std::memset(world_state.get(), 0, sizeof(World));
        game->map_width = map_w;
        game->map_height = map_h;
        game->feature_def_count = 3;
        game->feature_defs = oa_ref_from_index(0);
        world_state->feature_defs = feature_defs.data();
        world_state->feature_def_count = static_cast<uint32_t>(feature_defs.size());
        world_state->units = units.data();
        world_state->unit_slot_count = static_cast<uint32_t>(units.size());
        world_state->unit_defs = unit_defs.data();
        world_state->unit_def_count = static_cast<uint32_t>(unit_defs.size());
        hooks.context = this;
        hooks.resolve = [](void* c, SaveRef kind, oa_ref32) -> void* {
            auto* f = static_cast<Fixture*>(c);
            switch (kind) {
            case SaveRef::movement:
                return f->movement.data();
            case SaveRef::player_info:
                return &f->info;
            case SaveRef::mission_rules:
                return f->rules.data();
            }
            return nullptr;
        };
        hooks.write_script = [](void* c, Unit* u, Bank* bank) {
            static_cast<Fixture*>(c)->log.push_back("script " + std::to_string(u->id));
            bank_blob_write(bank, "S", 1);
        };
        hooks.visit_orders = [](void* c, const Unit* u, SavedOrderVisit visit, void* walk) {
            auto* f = static_cast<Fixture*>(c);
            f->log.push_back("orders " + std::to_string(u->id));
            if (u->id != 1)
                return;
            const SavedGoal none{};
            for (const auto* queue : {&f->primary, &f->secondary})
                for (const SavedOrder& order : *queue)
                    visit(walk, &order, &none);
        };
        hooks.restore_unit = [](void* c, uint16_t id, Bank*) {
            static_cast<Fixture*>(c)->log.push_back("restore " + std::to_string(id));
        };
        hooks.load_feature_set = [](void* c) {
            static_cast<Fixture*>(c)->log.push_back("load set");
        };
        hooks.find_or_load_feature = [](void* c, const char* name) -> int16_t {
            static_cast<Fixture*>(c)->log.push_back(std::string("find ") + name);
            return 7;
        };
        hooks.link_feature_set = [](void* c) {
            static_cast<Fixture*>(c)->log.push_back("link set");
        };
        hooks.place_feature =
            [](void* c, uint8_t* p, uint16_t type, const uint8_t* object, const uint8_t* extra) {
                auto* f = static_cast<Fixture*>(c);
                const auto cell = static_cast<std::size_t>(p - f->plots.data()) / plot_bytes;
                f->log.push_back(
                    "place " + std::to_string(cell) + " " + std::to_string(type) +
                    (object ? " obj" : "") + (extra ? " extra" : "")
                );
                p[plot::feature] = static_cast<uint8_t>(type);
                p[plot::feature + 1] = static_cast<uint8_t>(type >> 8);
                p[plot::feature_record] = static_cast<uint8_t>(cell % 8);
                p[plot::feature_record + 1] = 0;
            };
        hooks.burn_feature = [](void* c, uint16_t x, uint16_t z) {
            static_cast<Fixture*>(c)->log.push_back(
                "burn " + std::to_string(x) + "," + std::to_string(z)
            );
        };
        hooks.queue_feature_event = [](void* c, uint16_t x, uint16_t z, int32_t kind) {
            static_cast<Fixture*>(c)->log.push_back(
                "event " + std::to_string(x) + "," + std::to_string(z) + " " + std::to_string(kind)
            );
        };
        world = SaveContext{
            world_state.get(),
            plots.data(),
            mapping.data(),
            feature_records.data(),
            &meteor,
            &hooks,
            static_cast<uint32_t>(feature_records.size() / feature_record_bytes)
        };
    }

    uint8_t* plot_at(int x, int z) { return plots.data() + (z * map_w + x) * plot_bytes; }
};

struct ScopedBank {
    Bank bank{};

    ScopedBank() {
        bank_init(&bank);
        bank_reset(&bank);
    }

    ~ScopedBank() { bank_destroy(&bank); }

    Bank* get() { return &bank; }
};

// Saves are only read back from a loaded bank, where every blob starts at
// position zero; round-trip through a file image as the game does.
void reload(Bank* from, ScopedBank& to) {
    ByteImage image{};
    CHECK(bank_write_image(from, savegame_description, true, &image));
    BankError error{};
    CHECK(bank_read_image(to.get(), image.data, image.size, savegame_description, nullptr, &error));
    byte_image_free(&image);
}

std::vector<uint8_t> blob(Bank* bank, const char* account, const char* name) {
    bank_open_account(bank, account);
    if (!bank_open_blob_name(bank, name))
        return {};
    std::vector<uint8_t> out(static_cast<std::size_t>(bank_blob_size(bank)));
    bank_blob_seek(bank, 0);
    bank_blob_read(bank, out.data(), static_cast<uint32_t>(out.size()));
    return out;
}

void camera_and_meteor() {
    Fixture f;
    f.game->camera_x = 320;
    f.game->camera_y = 1600;
    f.meteor.enabled = 1;
    f.meteor.active = 1;
    f.meteor.next_strike_tick = 900;
    f.meteor.strike_end_tick = 1050;
    f.meteor.next_hit_tick = 930;
    f.meteor.origin_x = -5;
    f.meteor.origin_z = 12;
    f.meteor.target_x = 300;
    f.meteor.target_z = -7;
    ScopedBank bank;
    save_write_camera(&f.world, bank.get());
    save_write_meteor(&f.meteor, bank.get());
    Fixture g;
    save_read_camera(&g.world, bank.get());
    save_read_meteor(&g.meteor, bank.get());
    CHECK(g.game->camera_x == 320 && g.game->camera_y == 1600);
    CHECK(std::memcmp(&g.meteor, &f.meteor, sizeof(g.meteor)) == 0);
    // Absent camera fields keep the current position.
    ScopedBank empty;
    g.game->camera_x = 5;
    save_read_camera(&g.world, empty.get());
    CHECK(g.game->camera_x == 5);
}

void map_sections() {
    Fixture f;
    for (int i = 0; i < map_w * map_h; ++i) {
        f.plots[i * plot_bytes + plot::metal] = static_cast<uint8_t>(10 + i);
        f.plots[i * plot_bytes + plot::flags] = static_cast<uint8_t>(0x87 | ((i * 3) & 0xf) << 3);
        f.plots[i * plot_bytes + plot::height] = static_cast<uint8_t>(i * 2);
    }
    for (std::size_t i = 0; i < f.mapping.size(); ++i)
        f.mapping[i] = static_cast<uint8_t>(0xa0 + i);
    ScopedBank bank;
    save_write_metal_plotmap(&f.world, bank.get());
    save_write_player_features(&f.world, bank.get());
    save_write_terrain_mapping(&f.world, bank.get());
    const auto features = blob(bank.get(), "PlayerFeatures", "Plotmap");
    CHECK(features.size() == 6);
    // Cell 0 flags bits 3..6 = 0, cell 1 = 3.
    CHECK(features[0] == 0x03);
    CHECK(features[1] == static_cast<uint8_t>((6 << 4) | 9));
    CHECK(blob(bank.get(), "Metal", "Plotmap").size() == 12);
    CHECK(blob(bank.get(), "Mapping", "Data") == f.mapping);

    Fixture g;
    for (int i = 0; i < map_w * map_h; ++i)
        g.plots[i * plot_bytes + plot::flags] = 0x81;
    ScopedBank loaded;
    reload(bank.get(), loaded);
    save_read_metal_plotmap(&g.world, loaded.get());
    save_read_player_features(&g.world, loaded.get());
    save_read_terrain_mapping(&g.world, loaded.get());
    // Read in place, blobs sit at their end: nothing is restored.
    Fixture unread;
    save_read_metal_plotmap(&unread.world, bank.get());
    CHECK(unread.plots[plot::metal] == 0);
    for (int i = 0; i < map_w * map_h; ++i) {
        CHECK(g.plots[i * plot_bytes + plot::metal] == 10 + i);
        CHECK(g.plots[i * plot_bytes + plot::flags] == (0x81 | (((i * 3) & 0xf) << 3)));
    }
    CHECK(g.mapping == f.mapping);
    CHECK(save_cell_height(&f.world, 3, 2) == 22);
    CHECK(save_cell_height(&f.world, 4, 0) == 0);
    CHECK(save_cell_height(&f.world, -1, 0) == 0);

    // A size mismatch leaves the map untouched.
    Fixture small;
    small.game->map_width = 2;
    std::memset(small.plots.data(), 0, small.plots.size());
    save_read_metal_plotmap(&small.world, loaded.get());
    CHECK(small.plots[plot::metal] == 0);
}

void features() {
    Fixture f;
    oa::base::text::copy_terminated(f.feature_defs[0].name, "Tree");
    f.feature_defs[0].flags = feature_def_flag_sprite;
    oa::base::text::copy_terminated(f.feature_defs[1].name, "Wreck");
    f.feature_defs[1].flags = 0;
    oa::base::text::copy_terminated(f.feature_defs[2].name, "Burner");
    f.feature_defs[2].flags = feature_def_flag_sprite;
    f.feature_defs[2].seq_name_burn = 0x100;
    f.feature_defs[2].seq_name_die = 0x200;
    f.feature_defs[2].seq_name_reclamate = 0x300;
    for (int i = 0; i < map_w * map_h; ++i) {
        f.plots[i * plot_bytes + plot::feature] = 0xff;
        f.plots[i * plot_bytes + plot::feature + 1] = 0xff;
    }
    auto set = [&](int x, int z, uint16_t type, uint16_t record, uint8_t flags) {
        uint8_t* p = f.plot_at(x, z);
        p[plot::feature] = static_cast<uint8_t>(type);
        p[plot::feature + 1] = static_cast<uint8_t>(type >> 8);
        p[plot::feature_record] = static_cast<uint8_t>(record);
        p[plot::feature_record + 1] = static_cast<uint8_t>(record >> 8);
        p[plot::flags] = flags;
    };
    set(1, 0, 0, 0x1234, 0); // normal
    set(2, 1, 1, 3, 0);      // 3D
    set(0, 2, 2, 4, 1);      // animating, die sequence
    set(3, 2, 2, 5, 1);      // animating, unknown sequence: skipped
    set(0, 0, 0xfffe, 0, 0); // footprint continuation
    uint8_t* object = f.feature_records.data() + 3 * feature_record_bytes;
    for (std::size_t i = 0; i < feature_record_bytes; ++i)
        object[i] = static_cast<uint8_t>(0x40 + i);
    uint8_t* anim = f.feature_records.data() + 4 * feature_record_bytes;
    anim[feature_record::frame] = 9;
    anim[feature_record::spread_countdown] = 0xa7;
    anim[feature_record::damage] = 0x34;
    anim[feature_record::damage + 1] = 0x12;
    std::memcpy(anim + feature_record::sequence, "\x00\x02\x00\x00", 4);
    uint8_t* other = f.feature_records.data() + 5 * feature_record_bytes;
    std::memcpy(other + feature_record::sequence, "\x99\x00\x00\x00", 4);

    ScopedBank bank;
    save_write_features(&f.world, bank.get());
    bank_open_account(bank.get(), "Features");
    CHECK(bank_get_int(bank.get(), "Number of Normal Features", -1) == 1);
    CHECK(bank_get_int(bank.get(), "Number of 3D Features", -1) == 1);
    CHECK(bank_get_int(bank.get(), "Number of Animating Features", -1) == 1);
    const auto names = blob(bank.get(), "Features", "Feature Type Names");
    CHECK(names.size() == 3 * 0x80);
    CHECK(std::strcmp(reinterpret_cast<const char*>(names.data() + 0x80), "Wreck") == 0);
    const auto normal = blob(bank.get(), "Features", "Normal Features");
    CHECK((normal == std::vector<uint8_t>{1, 0, 0, 0, 0, 0, 0x34, 0x12}));
    const auto animating = blob(bank.get(), "Features", "Animating Features");
    CHECK((animating == std::vector<uint8_t>{0, 0, 2, 0, 2, 0, 0x34, 0x12, 9, 0xa1}));
    const auto objects = blob(bank.get(), "Features", "3D Features");
    CHECK(objects.size() == 0x1a);
    CHECK(objects[0] == 2 && objects[2] == 1 && objects[4] == 1);
    CHECK(
        objects[6] == object[feature_record::damage] &&
        objects[8] == object[feature_record::position]
    );
    CHECK(objects[24] == object[feature_record::orientation + 4]);

    // Reload with the defs reordered: saved names remap to current indices.
    Fixture g;
    oa::base::text::copy_terminated(g.feature_defs[0].name, "burner");
    oa::base::text::copy_terminated(g.feature_defs[1].name, "TREE");
    oa::base::text::copy_terminated(g.feature_defs[2].name, "Rock");
    ScopedBank loaded;
    reload(bank.get(), loaded);
    save_read_features(&g.world, loaded.get());
    const std::vector<std::string> expected{
        "load set",
        "find Wreck",
        "link set",
        "place 1 1",
        "place 8 0",
        "event 0,2 0",
        "place 6 7 obj extra"
    };
    CHECK(g.log == expected);
    CHECK(g.plot_at(1, 0)[plot::feature_record] == 0x34);
    const uint8_t* restored = g.feature_records.data() + (8 % 8) * feature_record_bytes;
    CHECK(restored[feature_record::frame] == 9);
    CHECK(restored[feature_record::spread_countdown] == 0xa0);
    CHECK(restored[feature_record::damage] == 0x34);
}

// Each animating record's last byte is the spread countdown's high nibble
// with the playing sequence below it, whichever records came before; the
// load restarts the sequence and keeps only that high nibble.
void animating_sequences() {
    Fixture f;
    oa::base::text::copy_terminated(f.feature_defs[2].name, "Burner");
    f.feature_defs[2].flags = feature_def_flag_sprite;
    f.feature_defs[2].seq_name_burn = 0x100;
    f.feature_defs[2].seq_name_die = 0x200;
    f.feature_defs[2].seq_name_reclamate = 0x300;
    for (int i = 0; i < map_w * map_h; ++i) {
        f.plots[i * plot_bytes + plot::feature] = 0xff;
        f.plots[i * plot_bytes + plot::feature + 1] = 0xff;
    }

    struct Case {
        uint16_t sequence;
        uint8_t countdown;
        uint8_t written;
    };

    // Cells 0..4 in row order use records 0..4.
    const Case cases[] = {
        {0x300, 0x2f, 0x22}, // reclamate first: nothing carries into the burn after it
        {0x100, 0x75, 0x70}, // burning, 117 ticks left
        {0x100, 0x0f, 0x00}, // burning, 15 ticks left
        {0x200, 0x9e, 0x91}, // die
        {0x100, 0x13, 0x10}, // burning, 19 ticks left
    };
    for (uint8_t i = 0; i < std::size(cases); ++i) {
        uint8_t* p = f.plots.data() + i * plot_bytes;
        p[plot::feature] = 2;
        p[plot::feature + 1] = 0;
        p[plot::feature_record] = i;
        p[plot::feature_record + 1] = 0;
        p[plot::flags] = plot_flag_animating_feature;
        uint8_t* record = f.feature_records.data() + i * feature_record_bytes;
        record[feature_record::frame] = static_cast<uint8_t>(10 + i);
        record[feature_record::damage] = static_cast<uint8_t>(20 + i);
        record[feature_record::spread_countdown] = cases[i].countdown;
        record[feature_record::sequence] = static_cast<uint8_t>(cases[i].sequence);
        record[feature_record::sequence + 1] = static_cast<uint8_t>(cases[i].sequence >> 8);
    }
    ScopedBank bank;
    save_write_features(&f.world, bank.get());
    const auto animating = blob(bank.get(), "Features", "Animating Features");
    CHECK(animating.size() == std::size(cases) * 10);
    for (std::size_t i = 0; i < std::size(cases) && i * 10 + 9 < animating.size(); ++i) {
        const uint8_t* record = animating.data() + i * 10;
        CHECK(record[0] == i % map_w && record[2] == i / map_w && record[4] == 2);
        CHECK(record[6] == 20 + i && record[8] == 10 + i);
        CHECK(record[9] == cases[i].written);
    }

    Fixture g;
    oa::base::text::copy_terminated(g.feature_defs[2].name, "Burner");
    ScopedBank loaded;
    reload(bank.get(), loaded);
    save_read_features(&g.world, loaded.get());
    const std::vector<std::string> expected{
        "load set",
        "link set",
        "place 0 2",
        "event 0,0 1",
        "place 1 2",
        "burn 1,0",
        "place 2 2",
        "burn 2,0",
        "place 3 2",
        "event 3,0 0",
        "place 4 2",
        "burn 0,1",
    };
    CHECK(g.log == expected);
    // The fixture's placement gives cell n record n % 8.
    for (uint8_t i = 0; i < std::size(cases); ++i) {
        const uint8_t* record = g.feature_records.data() + i * feature_record_bytes;
        CHECK(record[feature_record::spread_countdown] == (cases[i].written & 0xf0));
        CHECK(record[feature_record::frame] == 10 + i && record[feature_record::frame + 1] == 0);
        CHECK(record[feature_record::damage] == 20 + i);
    }
}

// A crafted section: normal records give two cells record indices at and
// past the eight records, then animating and 3D records for those cells name
// a type past the name table, so their placements leave the indices as the
// file set them. The sequences still start, and nothing is written through
// the indices.
void feature_record_bounds() {
    Fixture g;
    oa::base::text::copy_terminated(g.feature_defs[0].name, "Tree");
    oa::base::text::copy_terminated(g.feature_defs[1].name, "Wreck");
    oa::base::text::copy_terminated(g.feature_defs[2].name, "Burner");
    // A placement of no type leaves its plot as it was.
    g.hooks.place_feature =
        [](void* c, uint8_t* p, uint16_t type, const uint8_t* object, const uint8_t*) {
            auto* f = static_cast<Fixture*>(c);
            const auto cell = static_cast<std::size_t>(p - f->plots.data()) / plot_bytes;
            f->log.push_back(
                "place " + std::to_string(cell) + " " + std::to_string(type) +
                (object ? " obj" : "")
            );
            if (type == 0xffff)
                return;
            p[plot::feature] = static_cast<uint8_t>(type);
            p[plot::feature + 1] = static_cast<uint8_t>(type >> 8);
            p[plot::feature_record] = 0;
            p[plot::feature_record + 1] = 0;
        };
    std::memset(g.feature_records.data(), 0xcc, g.feature_records.size());
    const std::vector<uint8_t> untouched = g.feature_records;

    ScopedBank bank;
    bank_open_account(bank.get(), "Features");
    char names[3 * 0x80] = {};
    oa::base::text::copy_terminated(std::span(names, 0x80), "Tree");
    oa::base::text::copy_terminated(std::span(names + 0x80, 0x80), "Wreck");
    oa::base::text::copy_terminated(std::span(names + 0x100, 0x80), "Burner");
    bank_open_blob_name(bank.get(), "Feature Type Names");
    bank_blob_write(bank.get(), names, sizeof names);
    const uint8_t normal[] = {
        1,
        0,
        0,
        0,
        0,
        0,
        0xff,
        0x7f, // (1,0) Tree, record index 0x7fff
        2,
        0,
        0,
        0,
        0,
        0,
        8,
        0, // (2,0) Tree, record index 8
    };
    bank_set_int(bank.get(), "Number of Normal Features", 2);
    bank_open_blob_name(bank.get(), "Normal Features");
    bank_blob_write(bank.get(), normal, sizeof normal);
    const uint8_t animating[] = {
        1, 0, 0, 0, 99, 0, 0x34, 0x12, 9, 0x70, // (1,0) type 99, burning
        2, 0, 0, 0, 99, 0, 0x34, 0x12, 9, 0x01, // (2,0) type 99, dying
    };
    bank_set_int(bank.get(), "Number of Animating Features", 2);
    bank_open_blob_name(bank.get(), "Animating Features");
    bank_blob_write(bank.get(), animating, sizeof animating);
    uint8_t object[0x1a] = {1, 0, 0, 0, 99, 0, 0x78, 0x56};
    for (std::size_t i = 8; i < sizeof object; ++i)
        object[i] = static_cast<uint8_t>(i);
    bank_set_int(bank.get(), "Number of 3D Features", 1);
    bank_open_blob_name(bank.get(), "3D Features");
    bank_blob_write(bank.get(), object, sizeof object);

    ScopedBank loaded;
    reload(bank.get(), loaded);
    save_read_features(&g.world, loaded.get());
    const std::vector<std::string> expected{
        "load set",
        "link set",
        "place 1 0",
        "place 2 0",
        "place 1 65535",
        "burn 1,0",
        "place 2 65535",
        "event 2,0 0",
        "place 1 65535 obj",
    };
    CHECK(g.log == expected);
    CHECK(g.plot_at(1, 0)[plot::feature_record] == 0xff);
    CHECK(g.plot_at(1, 0)[plot::feature_record + 1] == 0x7f);
    CHECK(g.plot_at(2, 0)[plot::feature_record] == 8);
    CHECK(g.feature_records == untouched);

    // A context without records writes no object or animating feature.
    Fixture f;
    oa::base::text::copy_terminated(f.feature_defs[1].name, "Wreck");
    for (int i = 0; i < map_w * map_h; ++i) {
        f.plots[i * plot_bytes + plot::feature] = 0xff;
        f.plots[i * plot_bytes + plot::feature + 1] = 0xff;
    }
    f.plot_at(0, 0)[plot::feature] = 1;
    f.plot_at(0, 0)[plot::feature + 1] = 0;
    f.world.feature_record_count = 0;
    ScopedBank empty;
    save_write_features(&f.world, empty.get());
    bank_open_account(empty.get(), "Features");
    CHECK(bank_get_int(empty.get(), "Number of 3D Features", -1) == 0);
    CHECK(blob(empty.get(), "Features", "3D Features").empty());
}

void units() {
    Fixture f;
    oa::base::text::copy_terminated(f.unit_defs[0].unit_name, "ARMCOM");
    f.game->weapon_defs[1].weapon_id = 42;
    Unit& a = f.units[1];
    a.id = 1;
    a.flags = OA_UNIT_FLAG_LIVE | 0x2000 | 0xabc | 0x4000;
    a.build_flags = 0x1d;
    a.def = oa_ref_from_index(0);
    a.owner_index = 3;
    a.position = FixedVec3{0x10000, 0x20000, 0x30000};
    a.health = 900;
    a.veteran_level = 2;
    a.movement = ref(ref_movement, 0);
    a.weapons[0].def = oa_ref_from_index(1);
    a.weapons[0].reload = 15;
    a.weapons[0].flags = 0xff;
    a.attach_parent = oa_unit_ref_from_slot(2);
    a.last_attacker_id = 2;
    a.build_remaining = 0.5f;
    a.squad = 4;
    a.attach_piece = 6;
    // Two primary orders and one secondary; the second is another unit's,
    // so its blob is not written but it is still counted.
    constexpr uint8_t stop = 45, patrol = 29, build_weapon = 13;
    f.primary = {
        SavedOrder{.owner_id = 1, .kind = stop}, SavedOrder{.owner_id = 2, .kind = patrol}
    };
    f.secondary = {SavedOrder{.owner_id = 1, .kind = build_weapon, .flags = 4}};
    for (std::size_t i = 0; i < f.movement.size(); ++i)
        f.movement[i] = static_cast<uint8_t>(i);
    Unit& carrier = f.units[2];
    carrier.id = 2;
    carrier.flags = OA_UNIT_FLAG_LIVE;
    carrier.def = oa_ref_from_index(0);
    f.units[3].id = 3; // dead slot

    ScopedBank bank;
    save_write_units(&f.world, bank.get());
    bank_open_account(bank.get(), "Units");
    CHECK(bank_get_int(bank.get(), "Number of Units", 0) == 2);
    CHECK(bank_get_int(bank.get(), "Version", 0) == 0x11);
    const std::vector<std::string> expected{"script 1", "orders 1", "script 2", "orders 2"};
    CHECK(f.log == expected);
    CHECK(blob(bank.get(), "Units", "u0001m0000").size() == order_blob_bytes);
    CHECK(blob(bank.get(), "Units", "u0001m0001").empty());
    const auto weapon_order = blob(bank.get(), "Units", "u0001m0002");
    CHECK(
        weapon_order.size() == order_blob_bytes && weapon_order[order_blob::kind] == build_weapon &&
        weapon_order[order_blob::flags] == 4
    );
    bank_open_account(bank.get(), "Units");
    CHECK(std::strcmp(bank_get_text(bank.get(), "u0001m0002_name", ""), "BuildWeapon") == 0);
    CHECK(!bank_has_field(bank.get(), "u0001m0001_name"));
    bank_open_blob_id(bank.get(), 0);
    uint8_t record[unit_record_bytes];
    bank_blob_seek(bank.get(), 0);
    CHECK(bank_blob_read(bank.get(), record, unit_record_bytes) == unit_record_bytes);
    namespace r = unit_record;
    CHECK(std::strcmp(reinterpret_cast<const char*>(record), "ARMCOM") == 0);
    CHECK(record[r::owner] == 3 && record[r::id] == 1);
    CHECK(record[r::order_count] == 3 && record[r::has_movement] == 1);
    CHECK(record[r::position + 2] == 1 && record[r::position + 6] == 2);
    CHECK(record[r::health] == (900 & 0xff) && record[r::veteran_level] == 2);
    // Id 42, reload 15 and the kept flag bits: the same 24 bytes as before.
    const uint8_t weapon_slot[unit_record::weapon_bytes] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f,
    };
    CHECK(std::memcmp(record + r::weapons, weapon_slot, sizeof weapon_slot) == 0);
    CHECK(record[r::carrier_id] == 2 && record[r::carrier_slot] == 6);
    CHECK(record[r::linked_id] == 2);
    CHECK(record[r::squad] == 4);
    uint32_t flags = 0;
    std::memcpy(&flags, record + r::flags, 4);
    CHECK(flags == (0xd | (0xabc << 4) | 0x10000 | (0x10004000u << 6)));
    const auto mob = blob(bank.get(), "Units", "u0001mob");
    CHECK(
        mob.size() == mobility_blob_bytes && mob[0] == 8 && mob[0x21] == 0x29 &&
        mob[0x22] == (0x2e & 7)
    );
    CHECK(blob(bank.get(), "Units", "u0001acc").size() == 0x30);
    CHECK(blob(bank.get(), "Units", "Script0") == std::vector<uint8_t>{'S'});
    // The carrier has no carrier.
    bank_open_account(bank.get(), "Units");
    bank_open_blob_id(bank.get(), 1);
    bank_blob_seek(bank.get(), 0);
    bank_blob_read(bank.get(), record, unit_record_bytes);
    CHECK(record[r::carrier_slot] == unit_record_no_carrier && record[r::has_movement] == 0);

    Fixture g;
    save_read_units(&g.world, bank.get());
    CHECK((g.log == std::vector<std::string>{"restore 1", "restore 2"}));

    // Legacy 0xb6-byte records restore nothing (their id is cleared).
    ScopedBank legacy;
    bank_open_account(legacy.get(), "Units");
    bank_set_int(legacy.get(), "Version", 0x11);
    bank_set_int(legacy.get(), "Number of Units", 2);
    bank_open_blob_id(legacy.get(), 0);
    std::memset(record, 0, sizeof(record));
    record[r::id] = 9;
    bank_blob_write(legacy.get(), record, unit_record_legacy_bytes);
    Fixture h;
    save_read_units(&h.world, legacy.get());
    CHECK((h.log == std::vector<std::string>{"restore 0"}));

    // A forged count and forged ids visit only the stored records, in id
    // order, and add no blobs.
    ScopedBank forged_bank;
    bank_open_account(forged_bank.get(), "Units");
    bank_set_int(forged_bank.get(), "Version", 0x11);
    bank_set_int(forged_bank.get(), "Number of Units", 0x7fffffff);
    const int32_t forged_ids[] = {0x7ffffffe, 1, -3, 0, 0x7fffffff};
    for (std::size_t k = 0; k < std::size(forged_ids); ++k) {
        std::memset(record, 0, sizeof(record));
        record[r::id] = static_cast<uint8_t>(20 + k);
        bank_open_blob_id(forged_bank.get(), forged_ids[k]);
        bank_blob_write(forged_bank.get(), record, unit_record_bytes);
    }
    Fixture forged;
    save_read_units(&forged.world, forged_bank.get());
    CHECK((forged.log == std::vector<std::string>{"restore 23", "restore 21", "restore 20"}));
    const auto& units = forged_bank.get()->accounts->items[forged_bank.get()->accounts->open];
    CHECK(units.blob_count == static_cast<int32_t>(std::size(forged_ids)));
}

// The unit record this silo wrote before a stockpiled round in flight was
// refunded: type SILO, owner 2, id 9, weapon id 122, stockpile 4, no carrier.
// Every other byte is zero, including the packed flags (OA_UNIT_FLAG_LIVE is
// not one of the bits the record keeps).
std::array<uint8_t, unit_record_bytes> silo_record_before_refund() {
    std::array<uint8_t, unit_record_bytes> record{};
    record[0] = 'S';
    record[1] = 'I';
    record[2] = 'L';
    record[3] = 'O';
    record[unit_record::owner] = 2;
    record[unit_record::id] = 9;
    record[unit_record::weapons + weapon_record::weapon_id] = 122;
    record[unit_record::weapons + weapon_record::stockpile] = 4;
    record[unit_record::carrier_slot] = unit_record_no_carrier;
    return record;
}

uint8_t
saved_stockpile_byte(const std::array<uint8_t, unit_record_bytes>& record, std::size_t slot) {
    return record
        [unit_record::weapons + slot * unit_record::weapon_bytes + weapon_record::stockpile];
}

// A save with no projectile, and one with only shots of a weapon that does not
// stockpile, is the record above. A stockpiled round still in flight is added
// to the first slot that holds that weapon, once per launch, and never past
// saved_stockpile_limit. Encoding does not change the unit.
void stockpile_refunds() {
    std::array<Projectile, OA_PROJECTILE_CAPACITY> pool{};
    Fixture fixture;
    oa::base::text::copy_terminated(fixture.unit_defs[0].unit_name, "SILO");
    fixture.game->weapon_defs[1].weapon_id = 122;
    fixture.world_state->projectiles = pool.data();
    Unit& silo = fixture.units[1];
    silo.id = 9;
    silo.flags = OA_UNIT_FLAG_LIVE;
    silo.def = oa_ref_from_index(0);
    silo.owner_index = 2;
    silo.weapons[0].def = oa_ref_from_index(1);
    silo.weapons[0].stockpile = 4;
    Unit& other = fixture.units[2];
    other.id = 3;
    other.flags = OA_UNIT_FLAG_LIVE;
    const oa_ref32 silo_ref = world_unit_ref(fixture.world_state.get(), &silo);
    const oa_ref32 other_ref = world_unit_ref(fixture.world_state.get(), &other);

    const auto encode = [&fixture, &silo] {
        const uint8_t slot0 = silo.weapons[0].stockpile;
        const uint8_t slot2 = silo.weapons[2].stockpile;
        std::array<uint8_t, unit_record_bytes> record{};
        save_encode_unit_record(&fixture.world, &silo, 0, false, record.data());
        CHECK(silo.weapons[0].stockpile == slot0);
        CHECK(silo.weapons[2].stockpile == slot2);
        return record;
    };

    const auto place = [&pool](std::size_t index, oa_ref32 source, uint32_t created) {
        Projectile& shot = pool[index];
        shot = Projectile{};
        shot.def = oa_ref_from_index(1);
        shot.source = source;
        shot.created_tick = created;
    };

    const auto pinned = silo_record_before_refund();
    CHECK(encode() == pinned);

    fixture.game->projectile_count = 3;
    for (int32_t index = 0; index < 3; ++index)
        place(static_cast<std::size_t>(index), silo_ref, static_cast<uint32_t>(20 + index));
    CHECK(encode() == pinned);

    pool = {};
    fixture.game->weapon_defs[1].flags = OA_WEAPON_FLAG_STOCKPILE;
    fixture.game->projectile_count = 1;
    place(0, silo_ref, 20);
    auto one_round = encode();
    CHECK(saved_stockpile_byte(one_round, 0) == 5);
    one_round[unit_record::weapons + weapon_record::stockpile] = 4;
    CHECK(one_round == pinned);
    CHECK(silo.weapons[0].stockpile == 4);

    fixture.game->projectile_count = 2;
    place(1, silo_ref, pool[0].created_tick);
    CHECK(saved_stockpile_byte(encode(), 0) == 5);

    place(1, silo_ref, pool[0].created_tick + 1);
    CHECK(saved_stockpile_byte(encode(), 0) == 6);

    pool[0].flags = OA_PROJECTILE_FLAG_RETIRED;
    pool[1].flags = OA_PROJECTILE_FLAG_RETIRED;
    CHECK(encode() == pinned);

    pool[0].flags = 0;
    pool[0].source = other_ref;
    fixture.game->projectile_count = 1;
    place(1, silo_ref, 40);
    CHECK(encode() == pinned);

    silo.weapons[0].stockpile = 199;
    place(0, silo_ref, 1);
    place(1, silo_ref, 2);
    fixture.game->projectile_count = 2;
    CHECK(saved_stockpile_byte(encode(), 0) == saved_stockpile_limit);
    CHECK(silo.weapons[0].stockpile == 199);

    silo.weapons[0].stockpile = 5;
    silo.weapons[2].def = oa_ref_from_index(1);
    silo.weapons[2].stockpile = 7;
    fixture.game->projectile_count = 1;
    pool[1] = {};
    const auto both = encode();
    CHECK(saved_stockpile_byte(both, 0) == 6);
    CHECK(saved_stockpile_byte(both, 1) == 0);
    CHECK(saved_stockpile_byte(both, 2) == 7);
    CHECK(silo.weapons[0].stockpile == 5);
    CHECK(silo.weapons[2].stockpile == 7);
}

// A weapon id of 300 is stored in the slot's two id bytes. Restoring it as
// one byte keeps the high byte the definition already had; restoring it as
// 16 bits gives 300. An id of 42 comes back as 42 either way.
void weapon_id_widths() {
    namespace r = unit_record;
    const auto encoded = [](uint16_t id) {
        Fixture fixture;
        oa_weapon_def_set_id(&fixture.game->weapon_defs[1], id);
        Unit& unit = fixture.units[1];
        unit.id = 1;
        unit.weapons[0].def = oa_ref_from_index(1);
        std::array<uint8_t, unit_record_bytes> record{};
        save_encode_unit_record(&fixture.world, &unit, 0, false, record.data());
        return record;
    };
    const auto restore = [](const uint8_t* slot, WeaponIdWidth width, uint8_t high) -> WeaponDef {
        Fixture fixture;
        WeaponDef& def = fixture.game->weapon_defs[1];
        def.weapon_id_high = high;
        UnitWeapon weapon{};
        weapon.def = oa_ref_from_index(1);
        save_restore_unit_weapon(fixture.world_state.get(), slot, weapon, width);
        return def;
    };

    const auto id_42 = encoded(42);
    const uint8_t* slot_42 = id_42.data() + r::weapons;
    CHECK(slot_42[weapon_record::weapon_id] == 42);
    CHECK(slot_42[weapon_record::weapon_id + 1] == 0);
    const WeaponDef narrow_42 = restore(slot_42, WeaponIdWidth::byte, 0);
    CHECK(oa_weapon_def_id(&narrow_42) == 42);
    const WeaponDef wide_42 = restore(slot_42, WeaponIdWidth::wide, 0x7f);
    CHECK(oa_weapon_def_id(&wide_42) == 42);

    const auto id_300 = encoded(300);
    const uint8_t* slot_300 = id_300.data() + r::weapons;
    CHECK(slot_300[weapon_record::weapon_id] == 0x2c);
    CHECK(slot_300[weapon_record::weapon_id + 1] == 0x01);
    CHECK(slot_300[weapon_record::weapon_id + 2] == 0);
    CHECK(slot_300[weapon_record::weapon_id + 3] == 0);
    const WeaponDef narrow_300 = restore(slot_300, WeaponIdWidth::byte, 0x05);
    CHECK(narrow_300.weapon_id == 0x2c);
    CHECK(narrow_300.weapon_id_high == 0x05);
    const WeaponDef wide_300 = restore(slot_300, WeaponIdWidth::wide, 0x05);
    CHECK(oa_weapon_def_id(&wide_300) == 300);
}

// The record each unit's restore reads: the first record the walk reaches
// that names the unit, and none past a gap in the ids, a record cut short or
// "Number of Units".
void unit_record_walk() {
    namespace r = unit_record;
    const auto put = [](Bank* bank, int32_t blob_id, uint16_t unit_id, uint32_t bytes) {
        uint8_t record[unit_record_bytes] = {};
        record[r::id] = static_cast<uint8_t>(unit_id);
        record[r::id + 1] = static_cast<uint8_t>(unit_id >> 8);
        bank_open_blob_id(bank, blob_id);
        bank_blob_write(bank, record, bytes);
    };
    const auto blob_count = [](Bank* bank) {
        return bank->accounts->items[bank->accounts->open].blob_count;
    };

    ScopedBank bank;
    bank_open_account(bank.get(), "Units");
    bank_set_int(bank.get(), "Number of Units", 6);
    put(bank.get(), 0, 5, unit_record_bytes);
    bank_open_blob_name(bank.get(), "Script0");
    bank_blob_write(bank.get(), "S", 1);
    put(bank.get(), 1, 0x109, unit_record_bytes);
    put(bank.get(), 2, 5, unit_record_bytes);     // a second record of unit 5
    put(bank.get(), 3, 0x200, unit_record_bytes); // past the table
    put(bank.get(), 5, 20, unit_record_bytes);    // past the gap at 4
    const int32_t blobs = blob_count(bank.get());
    bank_open_account(bank.get(), "Camera");
    std::vector<int32_t> records(0x110, 7);
    save_find_unit_records(bank.get(), records);
    std::vector<int32_t> expected(0x110, -1);
    expected[5] = 0;
    expected[0x109] = 1;
    CHECK(records == expected);
    // The walk leaves the Units account open and adds no blobs.
    CHECK(bank_get_int(bank.get(), "Number of Units", 0) == 6);
    CHECK(blob_count(bank.get()) == blobs);

    // A record cut short ends the walk.
    ScopedBank short_bank;
    bank_open_account(short_bank.get(), "Units");
    bank_set_int(short_bank.get(), "Number of Units", 3);
    put(short_bank.get(), 0, 4, unit_record_bytes);
    put(short_bank.get(), 1, 6, unit_record_legacy_bytes);
    put(short_bank.get(), 2, 8, unit_record_bytes);
    std::vector<int32_t> cut(10);
    save_find_unit_records(short_bank.get(), cut);
    std::vector<int32_t> cut_expected(10, -1);
    cut_expected[4] = 0;
    CHECK(cut == cut_expected);

    // Records at and above "Number of Units" are not walked.
    ScopedBank counted;
    bank_open_account(counted.get(), "Units");
    bank_set_int(counted.get(), "Number of Units", 2);
    for (int32_t k = 0; k < 3; ++k)
        put(counted.get(), k, static_cast<uint16_t>(k + 1), unit_record_bytes);
    std::vector<int32_t> walked(4);
    save_find_unit_records(counted.get(), walked);
    CHECK((walked == std::vector<int32_t>{-1, 0, 1, -1}));
}

struct Summary {
    int advanced = 0;
    int bound = 0;
    std::vector<uint8_t> radar = std::vector<uint8_t>(6 * 4, 0x33);
    ImageRows rows{4, 6, 4, nullptr};
};

SummaryHooks summary_hooks(Summary* s) {
    SummaryHooks h{};
    h.context = s;
    h.build_date = "Nov 17 1998";
    h.build_time = "12:00:00";
    h.campaign_name = [](void*) { return "Core"; };
    h.advance_next_mission = [](void* c) { ++static_cast<Summary*>(c)->advanced; };
    h.mission_name = [](void*) { return "cormis1"; };
    h.game_type = [](void*) { return 2; };
    h.bind_mission_info = [](void* c) { ++static_cast<Summary*>(c)->bound; };
    h.radar_image = [](void* c) -> const ImageRows* {
        auto* s = static_cast<Summary*>(c);
        s->rows.pixels = s->radar.data();
        return &s->rows;
    };
    h.write_stats_panel = [](void*, Bank* bank) {
        bank_open_account(bank, "Stats");
        bank_set_int(bank, "Kills", 3);
    };
    h.save_conditions = [](void*, Bank* bank) {
        bank_open_account(bank, "VictoryCondition_DestroyAllUnits");
        bank_set_int(bank, "Satisfied", 0);
    };
    return h;
}

struct MemoryFiles {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
};

FileSink memory_sink(MemoryFiles* m) {
    return FileSink{m, [](void* c, const char* path, const uint8_t* data, std::size_t size) {
                        static_cast<MemoryFiles*>(c)->files.emplace_back(
                            path, std::vector<uint8_t>(data, data + size)
                        );
                        return true;
                    }};
}

// u%04xacc goes back into the unit it was written from; a missing blob
// leaves the accumulators alone and a short one overwrites only its bytes.
void unit_economy() {
    Unit saved{};
    saved.id = 0x2b;
    saved.economy.energy = ResourceAccumulator{1.5f, 2.25f, 3.0f, -1.0f, 4.5f, 5.75f};
    saved.economy.metal = ResourceAccumulator{0.5f, 0.25f, 0.125f, 1.0f, 2.0f, 3.0f};
    ScopedBank written;
    bank_open_account(written.get(), "Units");
    save_write_unit_economy(&saved, written.get());
    const float head = 8.5f;
    bank_open_blob_name(written.get(), "u0007acc");
    bank_blob_write(written.get(), &saved.economy.energy, sizeof(ResourceAccumulator));
    bank_blob_write(written.get(), &head, sizeof(head));
    ScopedBank bank;
    reload(written.get(), bank);
    CHECK(blob(bank.get(), "Units", "u002bacc").size() == 2 * sizeof(ResourceAccumulator));

    bank_open_account(bank.get(), "Units");
    Unit restored{};
    restored.id = 0x2b;
    CHECK(save_read_unit_economy(&restored, bank.get()));
    CHECK(
        std::memcmp(&restored.economy.energy, &saved.economy.energy, sizeof(ResourceAccumulator)) ==
        0
    );
    CHECK(
        std::memcmp(&restored.economy.metal, &saved.economy.metal, sizeof(ResourceAccumulator)) == 0
    );

    Unit absent{};
    absent.id = 0x2c;
    absent.economy.metal.gate = 9.0f;
    CHECK(!save_read_unit_economy(&absent, bank.get()));
    CHECK(absent.economy.metal.gate == 9.0f);

    Unit clipped{};
    clipped.id = 7;
    clipped.economy.metal = ResourceAccumulator{7.0f, 6.0f, 5.0f, 4.0f, 3.0f, 2.0f};
    CHECK(!save_read_unit_economy(&clipped, bank.get()));
    CHECK(clipped.economy.energy.last_requested == 5.75f);
    CHECK(clipped.economy.metal.produced == 8.5f && clipped.economy.metal.requested == 6.0f);
    CHECK(clipped.economy.metal.last_requested == 2.0f);
}

void whole_game() {
    Fixture f;
    f.game->mode = game_mode_in_match;
    f.game->tick = 4500;
    f.game->local_player_index = 1;
    f.game->players[1].info = ref(ref_info, 0);
    f.info.side = 1;
    f.game->max_units_setting = 250;
    f.game->player_count = 2;
    f.game->difficulty = 1;
    f.game->skirmish_info = ref(ref_rules, 0);
    std::memcpy(f.game->mission_results, "cormis1", 8);
    f.rules[skirmish_rules::commander_death] = 1;
    f.rules[skirmish_rules::location] = 5;
    oa::base::text::copy_terminated(f.feature_defs[0].name, "Tree");
    for (int i = 0; i < map_w * map_h; ++i)
        f.plots[i * plot_bytes + plot::feature + 1] = 0xff,
                                                 f.plots[i * plot_bytes + plot::feature] = 0xff;
    f.units[1].id = 1;
    f.units[1].flags = OA_UNIT_FLAG_LIVE;
    f.units[1].def = oa_ref_from_index(0);

    Summary s;
    const SummaryHooks hooks = summary_hooks(&s);
    MemoryFiles files;
    const FileSink sink = memory_sink(&files);
    const char* tokens[] = {"open-annihilation", "quicksave"};
    char path[64];
    CHECK(save_command_line_game(&f.world, &hooks, tokens, 2, &sink, path, sizeof(path)));
    CHECK(std::string(path) == "savegame/quicksave.sav");
    CHECK(!save_command_line_game(&f.world, &hooks, tokens, 1, &sink, path, sizeof(path)));
    CHECK(files.files.size() == 1);
    CHECK(s.advanced == 0 && s.bound == 0);

    const auto& image = files.files[0].second;
    Bank bank;
    bank_init(&bank);
    BankError error{};
    CHECK(bank_read_image(
        &bank,
        image.data(),
        static_cast<uint32_t>(image.size()),
        savegame_description,
        nullptr,
        &error
    ));
    const char* order[] = {
        "Summary",
        "Camera",
        "Stats",
        "Units",
        "Mapping",
        "Features",
        "PlayerFeatures",
        "Metal",
        "Meteor",
        "VictoryCondition_DestroyAllUnits"
    };
    CHECK(bank.accounts->count == 10);
    for (int i = 0; i < bank.accounts->count && i < 10; ++i)
        CHECK(std::strcmp(bank.accounts->items[i].name, order[i]) == 0);
    bank_open_account(&bank, "Summary");
    CHECK(bank_has_field(&bank, "BUILD DATE: Nov 17 1998"));
    CHECK(bank_get_int(&bank, "maxunits", 0) == 250);
    CHECK(bank_get_int(&bank, "Side", 0) == 1);
    CHECK(bank_get_int(&bank, "Players", 0) == 2);
    CHECK(bank_get_int(&bank, "Gametype", 0) == 2);
    CHECK(bank_get_int(&bank, "Location", 0) == 5);
    CHECK(bank_get_int(&bank, "Game ID", 0) == command_line_game_id);
    CHECK(bank_get_int(&bank, "Game Time", 0) == 4500);
    CHECK(std::strcmp(bank_get_text(&bank, "Description", ""), command_line_description) == 0);
    CHECK(std::strcmp(bank_get_text(&bank, "Thumbs", ""), "cormis1") == 0);
    CHECK(!bank_has_field(&bank, "BetweenMissions"));
    const auto radar = blob(&bank, "Summary", "Radar Image");
    CHECK(radar.size() == 8 + 24 && radar[0] == 4 && radar[4] == 6 && radar[8] == 0x33);

    // Fixture corpus: the image re-reads and re-writes to identical bytes.
    ByteImage again{};
    CHECK(bank_write_image(&bank, savegame_description, true, &again));
    CHECK(std::vector<uint8_t>(again.data, again.data + again.size) == image);
    byte_image_free(&again);
    bank_destroy(&bank);

    // Between missions only the summary is written.
    f.game->mode = 0;
    files.files.clear();
    CHECK(save_write_game(&f.world, &hooks, "x.sav", nullptr, 1, &sink));
    CHECK(s.advanced == 1 && s.bound == 1);
    bank_init(&bank);
    CHECK(bank_read_image(
        &bank,
        files.files[0].second.data(),
        static_cast<uint32_t>(files.files[0].second.size()),
        nullptr,
        nullptr,
        &error
    ));
    CHECK(bank.accounts->count == 1);
    bank_open_account(&bank, "Summary");
    CHECK(bank_get_int(&bank, "BetweenMissions", 0) == 1);
    CHECK(!bank_has_field(&bank, "Description"));
    bank_destroy(&bank);
}

void meteor_config() {
    struct Values {
        const char* weapon;
        int32_t radius;
        double density, duration, interval;
    } v{"Meteor", 300, 2, 5, 60};

    MeteorTdf tdf{
        &v,
        [](void* c, const char*, char* out, std::size_t n) {
            auto* v = static_cast<Values*>(c);
            if (v->weapon == nullptr)
                return false;
            std::snprintf(out, n, "%s", v->weapon);
            return true;
        },
        [](void* c, const char*, int32_t) { return static_cast<Values*>(c)->radius; },
        [](void* c, const char* key, double) {
            auto* v = static_cast<Values*>(c);
            return std::strcmp(key, "MeteorDensity") == 0    ? v->density
                   : std::strcmp(key, "MeteorDuration") == 0 ? v->duration
                                                             : v->interval;
        }
    };
    oa::sim::world_environment::MeteorSettings config{};
    CHECK(load_meteor_config(&tdf, &config) == MeteorConfigResult::loaded);
    CHECK(
        std::strcmp(config.weapon, "Meteor") == 0 && config.radius == 300 &&
        config.density == 2.0f && config.duration == 5.0f && config.interval == 60.0f
    );
    v.interval = 0;
    CHECK(load_meteor_config(&tdf, &config) == MeteorConfigResult::bogus);
    v.interval = 60;
    v.weapon = nullptr;
    CHECK(load_meteor_config(&tdf, &config) == MeteorConfigResult::bogus);
    CHECK(load_meteor_config(nullptr, &config) == MeteorConfigResult::missing);
}

} // namespace

int main() {
    camera_and_meteor();
    map_sections();
    features();
    animating_sequences();
    feature_record_bounds();
    units();
    weapon_id_widths();
    stockpile_refunds();
    unit_record_walk();
    unit_economy();
    whole_game();
    meteor_config();
    return oa::data::persist::test::finish("persist-sections");
}
