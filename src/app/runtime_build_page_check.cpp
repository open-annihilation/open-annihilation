// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Build pages driven through synthetic SDL input: a download page with its
// linked buttons, the weapon page of every unit that stockpiles, and the page
// each unit keeps.
#include "oa/app/runtime.hpp"
#include "oa/core/weapon_def.h"
#include "oa/core/world.h"
#include "oa/data/persist/save_sections.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/mission_types.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/frontend_renderer/gadget_draw.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"
#include "oa/ui/hud/order_panel.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdlib>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {
namespace {

// Ticks the lab gets to build one Flea at full resources (about 610).
constexpr int kBuildTickLimit = 1600;
// Ticks a unit gets to stockpile its first round at full resources.
constexpr int kStockpileTickLimit = 12000;
// Rounds a shift-click adds or takes away, as 3.1c does.
constexpr int32_t kShiftRounds = 5;
// How far west of the commander the unit page memory check puts its PeeWee, in pixels.
constexpr int32_t kPeeWeeOffset = 96;

[[noreturn]] void fail(std::string_view what, std::string_view how = {}) {
    throw std::runtime_error(
        "download build check: " + std::string(what) + (how.empty() ? "" : " ") + std::string(how)
    );
}

[[noreturn]] void stockpile_fail(std::string_view what, std::string_view how = {}) {
    throw std::runtime_error(
        "stockpile build check: " + std::string(what) + (how.empty() ? "" : " ") + std::string(how)
    );
}

[[noreturn]] void page_memory_fail(std::string_view what, std::string_view how = {}) {
    throw std::runtime_error(
        "unit page memory check: " + std::string(what) + (how.empty() ? "" : " ") + std::string(how)
    );
}

} // namespace

void Runtime::check_download_builds() {
    namespace hud = oa::ui::hud;
    namespace orders = oa::sim::match_runtime;
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        fail("needs the SDL renderer");
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    start_benchmark_skirmish();
    apply_output_mode();
    const auto type_of = [&](const char* name) {
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (type == 0 || type >= spawn_types_.size())
            fail("lacks", name);
        return type;
    };
    const auto lab = type_of("ARMLAB");
    const auto flea = type_of("ARMFLEA");
    const auto warrior = type_of("ARMWAR");
    auto& slots = match_->world().slots;
    uint16_t commander = 0;
    for (const auto& slot : slots)
        if (slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_) {
            commander = slot.unit_index;
            break;
        }
    if (commander == 0)
        fail("found no local commander");

    const auto place = [&](uint16_t type) -> uint16_t {
        const auto* placed = place_finished_structure(type, commander);
        if (placed == nullptr)
            fail("could not place", spawn_type_names_[type]);
        return placed->unit_index;
    };
    const auto factory = place(lab);

    auto& player = match_->world().players[match_local_player_];
    const auto tick = [&] {
        player.metal = player.metal_cap;
        player.energy = player.energy_cap;
        step_match_simulation();
    };
    bool running = true;
    const auto send = [&](SDL_EventType type, uint8_t button, float x, float y) {
        float window_x = 0.0F;
        float window_y = 0.0F;
        if (!frame_to_window(sdl_.renderer, x, y, &window_x, &window_y))
            fail(SDL_GetError());
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion.windowID = SDL_GetWindowID(sdl_.window);
            event.motion.x = window_x;
            event.motion.y = window_y;
        } else {
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = button;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
        }
        dispatch_event(event, running);
    };
    const auto click = [&](float x, float y, uint8_t button) {
        send(SDL_EVENT_MOUSE_MOTION, 0, x, y);
        send(SDL_EVENT_MOUSE_BUTTON_DOWN, button, x, y);
        send(SDL_EVENT_MOUSE_BUTTON_UP, button, x, y);
    };
    const auto gadget_index = [&](std::string_view name) -> std::size_t {
        if (!match_hud_)
            fail("has no order panel");
        const auto& gadgets = match_hud_->layout.gadgets;
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (gadgets[index].common.name == name)
                return index;
        fail("found no button", name);
    };
    const auto click_gadget = [&](std::string_view name, uint8_t button) {
        const auto point = hud_gadget_centre(gadget_index(name));
        click(static_cast<float>(point.x), static_cast<float>(point.y), button);
    };
    const auto select = [&](uint16_t unit) {
        center_camera_on_unit(unit);
        const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
        const auto on_screen = project_match_point(viewport, slots[unit].unit->position);
        click(static_cast<float>(on_screen.x), static_cast<float>(on_screen.y), SDL_BUTTON_LEFT);
        if (selected_match_unit_ != unit)
            fail("did not select", spawn_type_names_[slots[unit].record.type_index]);
    };
    const auto snapshot = [&](const char* name) {
        render_match_surface();
        renderer::Surface frame;
        compose_match_frame(frame);
        write_ppm(report_directory / name, frame);
    };
    const auto queue = [&](uint16_t unit, bool secondary) {
        std::vector<std::pair<int32_t, int32_t>> entries; // type, count
        std::array<orders::Match::OrderRecordView, 8> records{};
        const auto count = match_->queue_records(unit, secondary, records.data(), records.size());
        for (std::size_t index = 0; index < count; ++index)
            entries.emplace_back(records[index].parameter_1, records[index].parameter_2);
        return entries;
    };
    using Queue = std::vector<std::pair<int32_t, int32_t>>;

    select(factory);
    if (match_hud_panel_ != oa::data::defs::gui_path("ARMLAB1.GUI") || match_build_page_ != 1)
        fail("did not open ARMLAB1.GUI for the lab");
    click_gadget("ARMNEXT", SDL_BUTTON_LEFT);
    if (match_hud_panel_ != oa::data::defs::gui_path("ARMDL.GUI") || match_build_page_ != 2)
        fail("did not open the download page ARMDL.GUI on NEXT");
    for (const auto& [slot, name] : {std::pair{4U, "ARMWAR"}, std::pair{5U, "ARMFLEA"}}) {
        const auto& gadget = match_hud_->layout.gadgets.at(slot);
        const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
        if (gadget.common.name != name || !gadget.common.gaf_file || button == nullptr ||
            button->grayed_out ||
            static_cast<uint8_t>(gadget.common.common_attributes) != hud::kCommonUnitButton)
            fail("did not link a live button into the patch for", name);
        if (gaf_sequence(match_hud_->sprites, name) == nullptr)
            fail("did not load the _gadget art of", name);
    }
    for (std::size_t slot = 6; slot < 10; ++slot) {
        const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(
            &match_hud_->layout.gadgets.at(slot).fields
        );
        if (match_hud_->layout.gadgets[slot].common.name != "IGPATCH" || button == nullptr ||
            !button->grayed_out)
            fail("linked more than the lab's two download buttons");
    }
    snapshot("native-download-page.ppm");
    const renderer::Surface page_hud = match_hud_cpu_;

    click_gadget("ARMFLEA", SDL_BUTTON_LEFT);
    click_gadget("ARMWAR", SDL_BUTTON_LEFT);
    click_gadget("ARMFLEA", SDL_BUTTON_LEFT);
    if (queue(factory, false) != Queue{{flea, 1}, {warrior, 1}, {flea, 1}})
        fail("did not queue Flea, Warrior, Flea from the download page");
    refresh_build_page(false);
    if (build_captions_.at(4) != "+1" || build_captions_.at(5) != "+2")
        fail("did not count the queued builds on the download buttons");
    snapshot("native-download-queued.ppm");
    // With the game's fonts, as the check's preferences file leaves them,
    // each count is written as 3.1c writes a build button's caption: in
    // hattfont12, centred along the bottom of the button (attribs=32), where
    // button_caption_pen places it. Nothing else of the button changes.
    if (!game_text_settings().style.modern_fonts) {
        ensure_gui_font();
        if (gui_font_.sequences.empty())
            fail("did not load hattfont12.gaf");
        const auto& queued_hud = match_hud_cpu_;
        if (page_hud.width != queued_hud.width || page_hud.height != queued_hud.height)
            fail("drew the queued page's HUD at another size");
        const auto pixel = [](const renderer::Surface& surface, int32_t x, int32_t y) {
            const auto at =
                (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3U;
            return std::array<uint8_t, 3>{
                surface.rgb[at], surface.rgb[at + 1], surface.rgb[at + 2]
            };
        };
        constexpr int32_t gui_line_gap = 2;
        const int32_t line_height = renderer::gui_font_baseline(gui_font_) + gui_line_gap;
        for (const std::size_t slot : {4U, 5U}) {
            const auto& common = match_hud_->layout.gadgets.at(slot).common;
            const auto& caption = build_captions_.at(slot);
            const int32_t width = renderer::measure_gadget_glyphs(gui_font_, caption);
            const auto pen = oa::ui::gui_layout::button_caption_pen(
                {common.x, common.y, common.x + common.width - 1, common.y + common.height - 1},
                static_cast<uint32_t>(common.attributes),
                width,
                line_height,
                false
            );
            if (!pen || (static_cast<uint32_t>(common.attributes) &
                         oa::ui::gui_layout::attribute::bottom_caption) == 0)
                fail("found no caption along the bottom of", common.name);
            bool written = false;
            bool in_font_colour = false;
            const int32_t right = std::min<int32_t>(common.x + common.width, queued_hud.width);
            const int32_t bottom = std::min<int32_t>(common.y + common.height, queued_hud.height);
            for (int32_t y = std::max<int32_t>(common.y, 0); y < bottom; ++y)
                for (int32_t x = std::max<int32_t>(common.x, 0); x < right; ++x) {
                    const auto shown = pixel(queued_hud, x, y);
                    if (shown == pixel(page_hud, x, y))
                        continue;
                    if (x < pen->x || x >= pen->x + width || y < pen->y ||
                        y >= pen->y + line_height)
                        fail(
                            "wrote outside the caption's place on",
                            common.name + " at " + std::to_string(x) + "," + std::to_string(y)
                        );
                    written = true;
                    in_font_colour = in_font_colour || shown == oa::present::gui_font_color;
                }
            if (!written || !in_font_colour)
                fail("did not write the count in hattfont12 on", common.name);
        }
    }
    click_gadget("ARMWAR", SDL_BUTTON_RIGHT);
    if (queue(factory, false) != Queue{{flea, 1}, {flea, 1}})
        fail("did not take the Warrior out of the middle of the queue");
    click_gadget("ARMFLEA", SDL_BUTTON_RIGHT);
    if (queue(factory, false) != Queue{{flea, 1}})
        fail("did not take the last Flea off the queue");
    uint16_t product = 0;
    for (int step = 0; step < kBuildTickLimit && product == 0; ++step) {
        tick();
        for (const auto& slot : slots)
            if (slot.unit != nullptr && slot.record.type_index == flea &&
                slot.record.owner_index == match_local_player_ &&
                slot.record.build_remaining == 0.0F)
                product = slot.unit_index;
    }
    if (product == 0)
        fail("saw no Flea come out of the lab");

    std::cout << "download build check: ARMDL.GUI linked the lab's Warrior and Flea, their "
                 "queue took and lost orders from the middle and the end and built a Flea, and "
                 "the queued counts showed along the bottom of their buttons\n";
}

void Runtime::check_stockpile_builds() {
    namespace hud = oa::ui::hud;
    namespace orders = oa::sim::match_runtime;
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        stockpile_fail("needs the SDL renderer");
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    start_benchmark_skirmish();
    apply_output_mode();

    // Every type whose first weapon stockpiles, by its weapon file.
    std::vector<uint16_t> stockpilers;
    for (std::size_t index = 0; index < unit_definitions_.size(); ++index) {
        const auto* weapon = weapon_registry_.find(unit_definitions_[index].weapon1);
        if (weapon != nullptr && (weapon->flags & OA_WEAPON_FLAG_STOCKPILE) != 0)
            stockpilers.push_back(static_cast<uint16_t>(index + 1));
    }
    if (stockpilers.empty())
        stockpile_fail("found no unit type that stockpiles");

    const auto commander = [&] {
        for (const auto& slot : match_->world().slots)
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                slot.record.owner_index == match_local_player_)
                return slot.unit_index;
        stockpile_fail("found no local commander");
    };
    const auto anchor = commander();
    bool running = true;
    const auto send = [&](SDL_EventType type, uint8_t button, float x, float y) {
        float window_x = 0.0F;
        float window_y = 0.0F;
        if (!frame_to_window(sdl_.renderer, x, y, &window_x, &window_y))
            stockpile_fail(SDL_GetError());
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion.windowID = SDL_GetWindowID(sdl_.window);
            event.motion.x = window_x;
            event.motion.y = window_y;
        } else {
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = button;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
        }
        dispatch_event(event, running);
    };
    const auto click = [&](float x, float y, uint8_t button) {
        send(SDL_EVENT_MOUSE_MOTION, 0, x, y);
        send(SDL_EVENT_MOUSE_BUTTON_DOWN, button, x, y);
        send(SDL_EVENT_MOUSE_BUTTON_UP, button, x, y);
    };
    // The first gadget of the open panel that passes `wanted`, 0 for none.
    const auto find_gadget = [&](auto wanted) -> std::size_t {
        if (!match_hud_)
            return 0;
        const auto& gadgets = match_hud_->layout.gadgets;
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (wanted(gadgets[index]))
                return index;
        return 0;
    };
    const auto named_ending = [&](std::string_view suffix) {
        return find_gadget([&](const auto& gadget) {
            const std::string_view name = gadget.common.name;
            return name.size() >= suffix.size() &&
                   name.substr(name.size() - suffix.size()) == suffix;
        });
    };
    const auto weapon_button = [&] {
        return find_gadget([](const auto& gadget) {
            return gadget.common.active != 0 &&
                   (static_cast<uint8_t>(gadget.common.common_attributes) &
                    hud::kCommonWeaponButton) != 0;
        });
    };
    const auto click_gadget = [&](std::size_t index, uint8_t button) {
        const auto point = hud_gadget_centre(index);
        click(static_cast<float>(point.x), static_cast<float>(point.y), button);
    };
    const auto select = [&](uint16_t unit) {
        center_camera_on_unit(unit);
        const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
        const auto on_screen =
            project_match_point(viewport, match_->world().slots[unit].unit->position);
        click(static_cast<float>(on_screen.x), static_cast<float>(on_screen.y), SDL_BUTTON_LEFT);
        if (selected_match_unit_ != unit)
            stockpile_fail(
                "did not select", spawn_type_names_[match_->world().slots[unit].record.type_index]
            );
    };
    const auto snapshot = [&](const std::string& name) {
        render_match_surface();
        renderer::Surface frame;
        compose_match_frame(frame);
        write_ppm(report_directory / name, frame);
    };
    const auto rounds_queued = [&](uint16_t unit) { return match_->queued_build_count(unit, 0); };
    const auto caption = [&](std::size_t index) {
        refresh_build_page(false);
        return build_captions_.at(index);
    };
    const auto live_unit = [&](uint16_t unit) -> oa::Unit& {
        auto* found = oa::world_unit_at(&match_->state(), unit);
        if (found == nullptr)
            stockpile_fail("lost a unit it placed");
        return *found;
    };
    const auto first_weapon = [&](uint16_t unit) {
        return oa::world_weapon_def(&match_->state(), live_unit(unit).weapons[0].def);
    };
    const auto stockpile = [&](uint16_t unit) {
        return static_cast<int32_t>(live_unit(unit).weapons[0].stockpile);
    };
    const auto build_weapon = oa::data::mission_types::index_for_name("BUILDWEAPON");
    // Near the commander, else at the site nearest it anywhere on the map, as
    // a ship needs water.
    const auto place = [&](uint16_t type) -> uint16_t {
        if (const auto* slot = place_finished_structure(type, anchor))
            return slot->unit_index;
        const auto& origin = live_unit(anchor).position;
        const auto cell_x = origin.x >> 20;
        const auto cell_z = origin.z >> 20;
        const auto [map_width, map_height] = shown_map_size();
        const auto reach = static_cast<int32_t>(std::max(map_width, map_height)) / 16;
        const auto& footprint = spawn_types_.at(type);
        for (int32_t ring = 0; ring < reach; ++ring)
            for (int32_t dz = -ring; dz <= ring; ++dz)
                for (int32_t dx = -ring; dx <= ring; ++dx) {
                    if ((std::abs(dx) != ring && std::abs(dz) != ring) ||
                        !match_->building_site(type, cell_x + dx, cell_z + dz, 0))
                        continue;
                    const auto x =
                        static_cast<uint32_t>(((cell_x + dx) * 2 + footprint.footprint_x) * 8);
                    const auto z =
                        static_cast<uint32_t>(((cell_z + dz) * 2 + footprint.footprint_z) * 8);
                    oa::sim::unit_spawn::Request request;
                    request.player = match_local_player_;
                    request.type = type;
                    request.finished = true;
                    request.state = kGroundOccupancyState;
                    request.position = {
                        x << 16,
                        static_cast<uint32_t>(match_->map_height(x << 16, z << 16)) << 16,
                        z << 16
                    };
                    if (auto* created = match_->create(request); created && created->unit)
                        return created->unit_index;
                }
        stockpile_fail("could not place", spawn_type_names_.at(type));
    };

    // Selecting the unit opens its weapon page, and the page queues rounds.
    std::string checked;
    int tabbed = 0;
    std::vector<std::pair<uint16_t, std::string>> placed;
    for (const auto type : stockpilers) {
        const std::string name = spawn_type_names_.at(type);
        const auto unit = place(type);
        const auto* weapon = first_weapon(unit);
        if (weapon == nullptr || (weapon->flags & OA_WEAPON_FLAG_STOCKPILE) == 0)
            stockpile_fail(name, "has no stockpiling first weapon");
        select(unit);
        const auto button = weapon_button();
        snapshot("stockpile-" + name + "-selected.ppm");
        if (match_build_page_ != 1 || button == 0)
            stockpile_fail(name, "did not open its weapon page when selected");
        {
            const auto& gadget = match_hud_->layout.gadgets[button];
            const auto* fields = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
            if (fields == nullptr || fields->grayed_out ||
                gaf_sequence(match_hud_->sprites, gadget.common.name) == nullptr)
                stockpile_fail(name, "has no live, drawn weapon button");
        }
        click_gadget(button, SDL_BUTTON_LEFT);
        std::array<orders::Match::OrderRecordView, 2> records{};
        if (rounds_queued(unit) != 1 ||
            match_->queue_records(unit, true, records.data(), records.size()) != 1 ||
            records[0].kind != build_weapon || records[0].parameter_1 != 0)
            stockpile_fail(name, "did not queue one round as BuildWeapon");
        shift_held_by_check_ = true;
        click_gadget(button, SDL_BUTTON_LEFT);
        shift_held_by_check_ = false;
        if (rounds_queued(unit) != 1 + kShiftRounds ||
            caption(button) != " +" + std::to_string(1 + kShiftRounds))
            stockpile_fail(
                name, "did not add five rounds with a shift-click and count them on the button"
            );
        shift_held_by_check_ = true;
        click_gadget(button, SDL_BUTTON_RIGHT);
        shift_held_by_check_ = false;
        if (rounds_queued(unit) != 1)
            stockpile_fail(name, "did not take five rounds off with a shift right-click");
        click_gadget(button, SDL_BUTTON_RIGHT);
        if (rounds_queued(unit) != 0 || match_->queue_records(unit, true, records.data(), 1) != 0)
            stockpile_fail(name, "did not take the last round off with a right click");
        // A page that carries its own order buttons has no ORDERS tab.
        if (const auto orders_tab = named_ending("ORDERS"); orders_tab != 0) {
            click_gadget(orders_tab, SDL_BUTTON_LEFT);
            const auto build_tab = named_ending("BUILD");
            if (match_build_page_ != 0 || build_tab == 0)
                stockpile_fail(name, "did not go to the order page on ORDERS");
            click_gadget(build_tab, SDL_BUTTON_LEFT);
            if (match_build_page_ != 1 || weapon_button() == 0)
                stockpile_fail(name, "did not come back to its weapon page on BUILD");
            ++tabbed;
        }
        checked += (checked.empty() ? "" : ", ") + name;
        placed.emplace_back(unit, name);
    }

    // The cheapest round is built with the unit's own build power, and its
    // count and the queue behind it are kept through a save and a load.
    // Energy a unit of metal is worth when the rounds are compared.
    constexpr float energy_per_metal = 60.0F;
    const auto cost = [&](uint16_t unit) {
        const auto* weapon = first_weapon(unit);
        return weapon->energy_per_shot + weapon->metal_per_shot * energy_per_metal;
    };
    const auto cheapest =
        *std::min_element(placed.begin(), placed.end(), [&](const auto& a, const auto& b) {
            return cost(a.first) < cost(b.first);
        });
    const auto [builder, builder_name] = cheapest;
    select(builder);
    const auto button = weapon_button();
    click_gadget(button, SDL_BUTTON_LEFT);
    click_gadget(button, SDL_BUTTON_LEFT);
    auto& player = match_->world().players[match_local_player_];
    int ticks = 0;
    while (stockpile(builder) == 0 && ticks < kStockpileTickLimit) {
        player.metal = player.metal_cap;
        player.energy = player.energy_cap;
        step_match_simulation();
        ++ticks;
    }
    if (stockpile(builder) != 1 || rounds_queued(builder) != 1)
        stockpile_fail(builder_name, "did not stockpile one round and keep one queued");
    if (caption(button) != "1 +1")
        stockpile_fail(builder_name, "did not show one round held and one queued");
    snapshot("stockpile-built.ppm");
    const auto save = report_directory / "stockpile.sav";
    if (!save_match_game(
            save,
            oa::data::persist::command_line_description,
            oa::data::persist::command_line_game_id
        ))
        stockpile_fail("could not save:", status_);
    if (!load_saved_game(save) || !match_)
        stockpile_fail("could not load the save:", status_);
    const auto& restored = match_->world().slots.at(builder);
    if (restored.unit == nullptr ||
        spawn_type_names_.at(restored.record.type_index) != builder_name)
        stockpile_fail(builder_name, "was not restored in its slot");
    if (stockpile(builder) != 1 || rounds_queued(builder) != 1)
        stockpile_fail(builder_name, "lost its round or its queue in the save");
    select(builder);
    const auto loaded_button = weapon_button();
    if (match_build_page_ != 1 || loaded_button == 0 || caption(loaded_button) != "1 +1")
        stockpile_fail(builder_name, "did not show its round and queue after the load");
    snapshot("stockpile-loaded.ppm");
    std::cout << "stockpile build check: " << checked
              << " open their weapon page when selected and queue and drop rounds by one and "
                 "by five; "
              << tabbed << " went to ORDERS and back; " << builder_name << " stockpiled a round in "
              << ticks << " ticks and kept it and its queue through a save and a load\n";
}

void Runtime::check_unit_page_memory() {
    namespace hud = oa::ui::hud;
    using Point = oa::ui::display_layout::Point;
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        page_memory_fail("needs the SDL renderer");
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    start_benchmark_skirmish();
    apply_output_mode();

    uint16_t commander = 0;
    for (const auto& slot : match_->world().slots)
        if (slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_) {
            commander = slot.unit_index;
            break;
        }
    if (commander == 0)
        page_memory_fail("found no local commander");
    // A PeeWee, whose type has no build pages, beside the commander.
    const auto peewee_type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMPW");
    if (peewee_type == 0)
        page_memory_fail("found no ARMPW");
    oa::sim::unit_spawn::Request request;
    request.player = match_local_player_;
    request.type = peewee_type;
    request.finished = true;
    request.state = kGroundOccupancyState;
    {
        const auto& at = match_->world().slots[commander].unit->position;
        const auto x = static_cast<uint32_t>(static_cast<int32_t>(at[0] >> 16) - kPeeWeeOffset);
        const auto z = static_cast<uint32_t>(at[2] >> 16);
        request.position = {
            x << 16, static_cast<uint32_t>(match_->map_height(x << 16, z << 16)) << 16, z << 16
        };
    }
    const auto* spawned = match_->create(request);
    if (spawned == nullptr || spawned->unit == nullptr)
        page_memory_fail("could not spawn a PeeWee");
    const auto peewee = spawned->unit_index;
    const auto lab_type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMLAB");
    const auto* placed = lab_type != 0 ? place_finished_structure(lab_type, commander) : nullptr;
    if (placed == nullptr)
        page_memory_fail("could not place a Kbot Lab");
    const auto lab = placed->unit_index;
    const auto name_of = [&](uint16_t unit) {
        return std::string(spawn_type_names_.at(match_->world().slots.at(unit).record.type_index));
    };

    const auto click_at = [&](Point point, uint8_t button) {
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, point, 0);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, point, button);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, point, button);
    };
    const auto prefix = match_side_name_prefix();
    // The open panel's gadget <side prefix><action>, 0 for none.
    const auto gadget_named = [&](std::string_view action) -> std::size_t {
        if (!match_hud_)
            return 0;
        const auto name = prefix + std::string(action);
        const auto& gadgets = match_hud_->layout.gadgets;
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (gadgets[index].common.name == name)
                return index;
        return 0;
    };
    const auto press = [&](std::string_view action, std::string_view step) {
        const auto index = gadget_named(action);
        if (index == 0)
            page_memory_fail(
                std::string(step) + ": the panel has no", prefix + std::string(action)
            );
        const auto& common = match_hud_->layout.gadgets[index].common;
        click_at(
            oa::ui::display_layout::source_to_canvas(
                match_layout_, common.x + common.width / 2, common.y + common.height / 2
            ),
            SDL_BUTTON_LEFT
        );
    };
    // Presses a key; with `repeat`, the press is a held key's repeat.
    const auto key = [&](SDL_Keycode code, bool repeat = false) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.key = code;
        // The place a keyboard sends with the key: the action dispatcher
        // follows a held key by it.
        event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
        event.key.down = true;
        event.key.repeat = repeat;
        bool running = true;
        dispatch_event(event, running);
    };
    // Clicks the unit on the battlefield; with `add`, Shift is held and the
    // click adds it to the selection.
    const auto select = [&](uint16_t unit, bool add) {
        center_camera_on_unit(unit);
        const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
        const auto on_screen =
            project_match_point(viewport, match_->world().slots[unit].unit->position);
        if (add)
            SDL_SetModState(SDL_KMOD_LSHIFT);
        click_at({on_screen.x, on_screen.y}, SDL_BUTTON_LEFT);
        SDL_SetModState(SDL_KMOD_NONE);
        if ((match_->world().slots[unit].unit->flags & OA_UNIT_FLAG_SELECTED) == 0 ||
            (!add && selected_match_unit_ != unit))
            page_memory_fail("did not select", name_of(unit));
    };
    // The page a unit keeps in its flags: its build page, or 0 for its order page.
    const auto kept_page = [&](uint16_t unit) {
        const auto* found = oa::world_unit_at(&match_->state(), unit);
        if (found == nullptr)
            page_memory_fail("lost", name_of(unit));
        return (found->flags & hud::kUnitFlagBuildMenu) != 0
                   ? static_cast<int>(hud::build_page(found->flags))
                   : 0;
    };
    const auto general_page = oa::data::defs::gui_path(prefix + "GEN.GUI");
    // The panel shows `page` of the unit (0 its order page) and the unit keeps it.
    const auto expect = [&](uint16_t unit, int page, std::string_view step) {
        const bool on_general = match_hud_panel_ == general_page;
        if (match_build_page_ != page || (page == 0) != on_general)
            page_memory_fail(
                std::string(step) + ": the panel shows",
                (on_general ? std::string("the order page") : match_hud_panel_) + ", not " +
                    (page == 0 ? std::string("the order page") : "page " + std::to_string(page)) +
                    " of " + name_of(unit)
            );
        if (kept_page(unit) != page)
            page_memory_fail(
                std::string(step) + ":",
                name_of(unit) + " keeps page " + std::to_string(kept_page(unit)) + ", not " +
                    std::to_string(page)
            );
    };
    const auto snapshot = [&](const std::string& name) {
        render_match_surface();
        renderer::Surface frame;
        compose_match_frame(frame);
        write_ppm(report_directory / name, frame);
    };

    // The page keys sound on every press, as PREV and NEXT do.
    std::vector<std::string> heard;
    heard_interface_sounds_ = &heard;

    struct StopListening {
        Runtime& runtime;

        ~StopListening() { runtime.heard_interface_sounds_ = nullptr; }
    } stop_listening{*this};

    const auto page_key = [&](SDL_Keycode code, std::string_view step, bool repeat = false) {
        const auto sounds = std::count(heard.begin(), heard.end(), "nextbuildmenu");
        key(code, repeat);
        if (std::count(heard.begin(), heard.end(), "nextbuildmenu") != sounds + 1)
            page_memory_fail(std::string(step) + ":", "the key did not play nextbuildmenu once");
    };
    // The page count of a unit's type; pages run from 1 to one less.
    const auto page_count_of = [&](uint16_t unit) {
        return match_->state()
            .unit_defs[match_->world().slots.at(unit).record.type_index]
            .gui_page_count;
    };
    const int last_page = static_cast<int>(page_count_of(commander)) - 1;
    if (last_page < 3)
        page_memory_fail("needs a commander of three build pages or more");

    // ORDERS, BUILD and the page buttons change what the unit shows, and
    // selecting it again opens that.
    select(commander, false);
    expect(commander, 1, "selecting the commander");
    press("NEXT", "NEXT");
    expect(commander, 2, "NEXT");
    press("ORDERS", "ORDERS");
    expect(commander, 0, "ORDERS");
    select(lab, false);
    expect(lab, 1, "selecting the lab");
    select(commander, false);
    expect(commander, 0, "selecting the commander after ORDERS");
    snapshot("unit-page-memory-orders-kept.ppm");
    press("BUILD", "BUILD");
    expect(commander, 2, "BUILD after ORDERS");

    // PREV and NEXT turn one page at each press, from the first page to the
    // last and back.
    press("PREV", "PREV");
    expect(commander, 1, "PREV");
    press("PREV", "PREV on the first page");
    expect(commander, last_page, "PREV on the first page");
    press("PREV", "PREV again");
    expect(commander, last_page - 1, "PREV again");
    press("NEXT", "NEXT after PREV");
    expect(commander, last_page, "NEXT after PREV");
    press("NEXT", "NEXT on the last page");
    expect(commander, 1, "NEXT on the last page");

    // The page keys take the order page into their turn: back from the first
    // page and on from the last, and from it to the first page and the last.
    page_key(SDLK_COMMA, "the previous-page key on the first page");
    expect(commander, 0, "the previous-page key on the first page");
    page_key(SDLK_COMMA, "the previous-page key on the order page");
    expect(commander, last_page, "the previous-page key on the order page");
    page_key(SDLK_PERIOD, "the next-page key on the last page");
    expect(commander, 0, "the next-page key on the last page");
    page_key(SDLK_PERIOD, "the next-page key on the order page");
    expect(commander, 1, "the next-page key on the order page");
    page_key(SDLK_PERIOD, "the next-page key on the first page");
    expect(commander, 2, "the next-page key on the first page");

    // A held page key goes on turning: each repeat turns a page and plays
    // nextbuildmenu, through the order page as its press does. Once round
    // each way ends on the page it started from.
    const auto hold_page_key = [&](SDL_Keycode code, int step, std::string_view name) {
        const int pages = last_page + 1;
        int page = kept_page(commander);
        for (int repeat = 0; repeat < pages; ++repeat) {
            page_key(code, name, true);
            page = (page + step + pages) % pages;
            expect(commander, page, name);
        }
    };
    hold_page_key(SDLK_PERIOD, 1, "a repeat of the held next-page key");
    hold_page_key(SDLK_COMMA, -1, "a repeat of the held previous-page key");

    select(lab, false);
    expect(lab, 1, "selecting the lab again");
    press("ORDERS", "the lab's ORDERS");
    expect(lab, 0, "the lab's ORDERS");
    select(commander, false);
    expect(commander, 2, "selecting the commander after the keys");

    // A save and a load keep each unit's page in its flags.
    const auto save = report_directory / "unit-page-memory.sav";
    if (!save_match_game(
            save,
            oa::data::persist::command_line_description,
            oa::data::persist::command_line_game_id
        ))
        page_memory_fail("could not save:", status_);
    if (!load_saved_game(save) || !match_)
        page_memory_fail("could not load the save:", status_);
    for (const auto unit : {commander, lab, peewee})
        if (match_->world().slots.at(unit).unit == nullptr)
            page_memory_fail("lost a unit in its slot through the save");
    if (kept_page(commander) != 2 || kept_page(lab) != 0)
        page_memory_fail("lost the units' pages through the save");
    select(lab, false);
    expect(lab, 0, "selecting the lab after the load");
    select(commander, false);
    expect(commander, 2, "selecting the commander after the load");
    snapshot("unit-page-memory-loaded.ppm");

    // A PeeWee has no build pages: the page keys turn the page in its flags
    // and its panel stays on the general page, through reselection too.
    select(peewee, false);
    expect(peewee, 0, "selecting the PeeWee");
    const auto peewee_flags = [&] {
        const auto* found = oa::world_unit_at(&match_->state(), peewee);
        if (found == nullptr)
            page_memory_fail("lost the PeeWee");
        return found->flags;
    };
    const auto peewee_pages = page_count_of(peewee);
    if (peewee_pages != 0)
        page_memory_fail("ARMPW has build pages");
    const auto turned = hud::build_menu_forward(peewee_flags(), peewee_pages, true);
    page_key(SDLK_PERIOD, "the next-page key on the PeeWee");
    const auto expect_general = [&](std::string_view step) {
        if (match_build_page_ != 0 || match_hud_panel_ != general_page)
            page_memory_fail(
                std::string(step) + ": the panel shows",
                match_hud_panel_ + ", not the PeeWee's general page"
            );
    };
    expect_general("the next-page key on the PeeWee");
    if (peewee_flags() != turned)
        page_memory_fail("the next-page key did not turn the page in the PeeWee's flags");
    select(lab, false);
    select(peewee, false);
    expect_general("selecting the PeeWee after the next-page key");
    snapshot("unit-page-memory-peewee.ppm");

    // With two selected the panel shows the general page, BUILD and ORDERS
    // greyed, and no unit's page turns.
    select(commander, false);
    select(lab, true);
    if (selected_match_unit_ != commander || match_panel_unit() != nullptr)
        page_memory_fail("did not add the lab to the selection");
    if (match_build_page_ != 0 || match_hud_panel_ != general_page)
        page_memory_fail("did not show the general page for two units");
    for (const auto action : {"BUILD", "ORDERS"}) {
        const auto index = gadget_named(action);
        if (index == 0 || index >= match_hud_states_.size() || !match_hud_states_[index].grayed)
            page_memory_fail(prefix + action, "is not greyed for two units");
    }
    page_key(SDLK_PERIOD, "the next-page key for two units");
    press("BUILD", "BUILD for two units");
    if (match_build_page_ != 0 || kept_page(commander) != 2 || kept_page(lab) != 0)
        page_memory_fail("turned a page with two units selected");
    snapshot("unit-page-memory-two-units.ppm");
    select(lab, false);
    expect(lab, 0, "selecting the lab alone");
    select(commander, false);
    expect(commander, 2, "selecting the commander alone");
    std::cout << "unit page memory check: " << name_of(commander) << " and " << name_of(lab)
              << " kept the pages ORDERS, BUILD, PREV, NEXT and the page keys chose through "
                 "reselection and a save and a load; the keys turned through the order page "
                 "with nextbuildmenu, a held key a page with each repeat, "
              << name_of(peewee)
              << " kept its general page, and two selected showed the general page with BUILD "
                 "and ORDERS greyed and turned no page\n";
}

} // namespace oa::app
