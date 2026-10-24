// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The headless checks of the Pause key, the menus' hold on the match clock
// and the team panels, over the navigation check's skirmish, and of a
// skirmish saved paused and loaded again.
#include "oa/app/runtime.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/persist/save_sections.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/hud/share_panel.hpp"
#include "oa/ui/hud/team_panels.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {

namespace {

// Frames of a checked second of the match clock, and the clock time of each.
constexpr uint32_t kCheckFrameMs = 25;
constexpr uint32_t kCheckFramesPerSecond = 1000 / kCheckFrameMs;
// Map pixels right of the local commander the share check puts its kbot at.
constexpr int32_t kGiftSpread = 48;
// The metal the local player has when the share check opens SHARE.GUI.
constexpr float kGiftMetal = 300.0F;
// Cells of the sight grid the share check marks mapped by the local player alone.
constexpr std::size_t kMappedCells = 256;
// PlayerSetupInfo.state of a human player still in the game.
constexpr uint8_t kHumanPlaying = 1;
// PlayerSetupInfo.role bit of the player hosting the game.
constexpr uint8_t kHostRole = 0x01;

/// Records what the team panels tell the other players' machines.
struct ToldMachines {
    std::vector<std::string> log;
};

/// Returns a player's name.
///
/// @param player player record
/// @return Player.name up to its terminator or its size
std::string player_name(const oa::Player& player) {
    return {player.name, strnlen(player.name, sizeof player.name)};
}

} // namespace

void Runtime::check_pause_key() {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("pause key check: " + what);
    };
    require(screen_ == Screen::match && match_ && !match_finished_, "needs a running match");
    require(!match_paused_, "a menu is open");
    auto& game = match_->state().game;
    namespace console = oa::ui::console;
    require((game.sim_run_flags & console::kSimRunPaused) == 0, "the skirmish starts paused");
    bool running = true;
    const auto key = [&](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = code;
        event.key.scancode = scancode;
        handle_sdl_event(event, running);
    };
    uint32_t clock_ms = 1000;
    // Game ticks a second of 25 ms frames runs, through the frame's gate and
    // the clock as idle_tick has them.
    const auto ticks_in_one_second = [&] {
        match_timing_.previous_clock =
            oa::base::game_loop::scaled_clock(clock_ms, match_clock_scale());
        match_timing_.remainder = 0.0F;
        const auto before = match_timing_.tick;
        for (uint32_t frame = 0; frame < kCheckFramesPerSecond; ++frame) {
            clock_ms += kCheckFrameMs;
            if (match_clock_steps())
                advance_match_clock(clock_ms);
        }
        return match_timing_.tick - before;
    };
    renderer::Surface frame;
    const auto band = match_layout_.top;
    const CanvasRect title{
        (match_layout_.width + match_layout_.left) / 2 - band,
        match_layout_.height / 2 - band / 2,
        2 * band,
        band
    };
    const auto title_pixels = [&] {
        render_match_surface();
        compose_match_frame(frame);
        return copy_rect(frame, title);
    };
    clear_local_selection();
    reset_match_command();
    pending_build_type_ = 0;
    apply_match_hud_for_selection();
    const auto panel = match_hud_panel_;

    // Pause sets the bit, opens nothing and holds the clock; the title shows.
    const auto unpaused = title_pixels();
    key(SDLK_PAUSE, SDL_SCANCODE_PAUSE);
    require((game.sim_run_flags & console::kSimRunPaused) != 0, "Pause did not set the pause bit");
    require(!match_paused_ && match_hud_panel_ == panel, "Pause opened a menu");
    const auto paused_ticks = ticks_in_one_second();
    require(
        paused_ticks == 0, "the paused skirmish ran " + std::to_string(paused_ticks) + " ticks"
    );
    const auto title_changed = changed_pixels(unpaused, title_pixels());
    require(title_changed >= kTextMinPixels, "the paused title did not show");
    key(SDLK_PAUSE, SDL_SCANCODE_PAUSE);
    require((game.sim_run_flags & console::kSimRunPaused) == 0, "Pause again kept the pause bit");
    const auto resumed_ticks = ticks_in_one_second();
    require(resumed_ticks > 0, "Pause again did not resume the skirmish");

    // Resume must release an explicit Pause, not only the menu's hold.
    // Canceling a surrender dialog must leave that explicit pause intact.
    key(SDLK_PAUSE, SDL_SCANCODE_PAUSE);
    request_match_close();
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(
        !match_paused_ && (game.sim_run_flags & console::kSimRunPaused) != 0 &&
            ticks_in_one_second() == 0,
        "canceling surrender cleared the explicit pause"
    );
    key(SDLK_F2, SDL_SCANCODE_F2);
    require(match_paused_, "F2 did not open the menu over an explicit pause");
    activate_pause_gadget("OK");
    require(
        !match_paused_ && (game.sim_run_flags & console::kSimRunPaused) == 0 &&
            ticks_in_one_second() > 0,
        "Resume closed the menu but left the simulation paused"
    );
    key(SDLK_PAUSE, SDL_SCANCODE_PAUSE);
    key(SDLK_F2, SDL_SCANCODE_F2);
    key(SDLK_F2, SDL_SCANCODE_F2);
    require(
        !match_paused_ && (game.sim_run_flags & console::kSimRunPaused) == 0 &&
            ticks_in_one_second() > 0,
        "F2 closed the menu but left the simulation paused"
    );
#if defined(__APPLE__)
    // A desktop Mac can reach the same pause control without a Pause key.
    const auto command_pause = [&] {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = SDLK_P;
        event.key.scancode = SDL_SCANCODE_P;
        event.key.mod = SDL_KMOD_GUI;
        event.key.down = true;
        dispatch_event(event, running);
    };
    command_pause();
    require(
        !match_paused_ && (game.sim_run_flags & console::kSimRunPaused) != 0 &&
            ticks_in_one_second() == 0,
        "Cmd+P did not pause the desktop match"
    );
    command_pause();
    require(
        !match_paused_ && (game.sim_run_flags & console::kSimRunPaused) == 0 &&
            ticks_in_one_second() > 0,
        "Cmd+P did not resume the desktop match"
    );
#endif

    // F2 opens the options menu, which holds a skirmish; Escape opens no menu.
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(!match_paused_, "Escape opened a menu");
    key(SDLK_F2, SDL_SCANCODE_F2);
    require(
        match_paused_ && match_hud_panel_ == oa::data::defs::gui_path("ARMOPT.GUI"),
        "F2 did not open ARMOPT.GUI"
    );
    require(
        !match_clock_steps() && ticks_in_one_second() == 0,
        "the options menu did not hold the skirmish"
    );
    // Pause works while the menu holds the skirmish, and a save made then
    // keeps it, though the held clock has not stepped since.
    const auto saved_paused = [&] {
        return (saved_match_timing().flags & console::kSimRunPaused) != 0;
    };
    key(SDLK_PAUSE, SDL_SCANCODE_PAUSE);
    require(
        match_paused_ && (game.sim_run_flags & console::kSimRunPaused) != 0 && saved_paused(),
        "Pause inside ARMOPT.GUI is not what a save stores"
    );
    key(SDLK_PAUSE, SDL_SCANCODE_PAUSE);
    require(!saved_paused(), "a save after Pause again inside ARMOPT.GUI stores the pause");
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(!match_paused_, "Escape did not close ARMOPT.GUI");

    // 'h' moves nothing and Tab opens no team menu outside a multiplayer game.
    std::vector<float> stores;
    for (const auto& player : game.players) {
        const float metal = player.metal;
        const float energy = player.energy;
        stores.push_back(metal);
        stores.push_back(energy);
    }
    key(SDLK_H, SDL_SCANCODE_H);
    std::vector<float> after;
    for (const auto& player : game.players) {
        const float metal = player.metal;
        const float energy = player.energy;
        after.push_back(metal);
        after.push_back(energy);
    }
    require(stores == after, "'h' moved resources in a skirmish");
    require(
        !match_paused_ && !team_panel_open() && match_hud_panel_ == panel, "'h' opened a panel"
    );
    key(SDLK_TAB, SDL_SCANCODE_TAB);
    require(!match_paused_ && !team_panel_open(), "Tab opened the team menu in a skirmish");
    std::cout << "pause key check: " << resumed_ticks
              << " ticks a second, none while paused, paused title " << title_changed
              << " pixels, ARMOPT holds the skirmish and a save there keeps the Pause key's "
                 "bit, 'h' and Tab open nothing\n";
}

void Runtime::check_paused_save() {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("paused save check: " + what);
    };
    namespace console = oa::ui::console;
    start_benchmark_skirmish();
    require(!match_paused_ && !match_finished_, "needs a running skirmish");
    bool running = true;
    const auto key = [&](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = code;
        event.key.scancode = scancode;
        handle_sdl_event(event, running);
    };
    uint32_t clock_ms = 1000;
    // Game ticks a second of 25 ms frames runs, through the frame's gate and
    // the clock as idle_tick has them.
    const auto ticks_in_one_second = [&] {
        match_timing_.previous_clock =
            oa::base::game_loop::scaled_clock(clock_ms, match_clock_scale());
        match_timing_.remainder = 0.0F;
        const auto before = match_timing_.tick;
        for (uint32_t frame = 0; frame < kCheckFramesPerSecond; ++frame) {
            clock_ms += kCheckFrameMs;
            if (match_clock_steps())
                advance_match_clock(clock_ms);
        }
        return match_timing_.tick - before;
    };
    const auto pause_bit = [&] {
        return (match_->state().game.sim_run_flags & console::kSimRunPaused) != 0;
    };
    require(ticks_in_one_second() > 0, "the skirmish does not run");

    // Paused with the Pause key, then saved from the in-game menu, which
    // writes the pause bit into the save.
    key(SDLK_PAUSE, SDL_SCANCODE_PAUSE);
    require(pause_bit(), "Pause did not set the pause bit");
    key(SDLK_F2, SDL_SCANCODE_F2);
    require(match_paused_, "F2 did not open the in-game menu");
    require(
        (saved_match_timing().flags & console::kSimRunPaused) != 0,
        "a save made while paused does not store the pause"
    );
    const fs::path save = "paused.sav";
    if (!save_match_game(
            save,
            oa::data::persist::command_line_description,
            oa::data::persist::command_line_game_id
        ))
        require(false, "could not save: " + status_);
    const auto saved_tick = match_timing_.tick;

    // Loaded over the menu, as LOADGAME loads it, the game runs.
    if (!load_saved_game(save) || !match_)
        require(false, "could not load the save: " + status_);
    require(match_timing_.tick == saved_tick, "the load did not restore the saved tick");
    require(!match_paused_, "the loaded game opened a menu");
    require(
        !pause_bit() && (match_timing_.flags & console::kSimRunPaused) == 0,
        "the loaded game kept the pause bit"
    );
    const auto loaded_ticks = ticks_in_one_second();
    require(loaded_ticks > 0, "the loaded game ran " + std::to_string(loaded_ticks) + " ticks");

    // Pause holds the loaded game and Pause again lets it go.
    key(SDLK_PAUSE, SDL_SCANCODE_PAUSE);
    require(pause_bit(), "Pause did not pause the loaded game");
    const auto paused_ticks = ticks_in_one_second();
    require(
        paused_ticks == 0, "the paused loaded game ran " + std::to_string(paused_ticks) + " ticks"
    );
    key(SDLK_PAUSE, SDL_SCANCODE_PAUSE);
    require(!pause_bit(), "Pause again kept the pause bit");
    const auto resumed_ticks = ticks_in_one_second();
    require(resumed_ticks > 0, "Pause again did not resume the loaded game");

    // F2 holds it with the in-game menu and F2 again lets it go.
    key(SDLK_F2, SDL_SCANCODE_F2);
    require(match_paused_ && ticks_in_one_second() == 0, "the in-game menu did not hold the game");
    key(SDLK_F2, SDL_SCANCODE_F2);
    require(
        !match_paused_ && ticks_in_one_second() > 0, "closing the menu did not resume the game"
    );
    std::cout << "paused save check: saved paused at tick " << saved_tick << ", loaded running "
              << loaded_ticks << " ticks a second, none while paused again, " << resumed_ticks
              << " after Pause again, and the in-game menu holds it and lets it go\n";
}

void Runtime::check_team_panels() {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("team panel check: " + what);
    };
    require(
        screen_ == Screen::match && match_ && !match_finished_ && !match_paused_,
        "needs a running match with no menu open"
    );
    auto& world = match_->state();
    auto& game = world.game;
    namespace console = oa::ui::console;
    const uint8_t local = game.local_player_index;
    require(local < OA_PLAYER_COUNT, "no local player");
    uint8_t computer = OA_PLAYER_COUNT;
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index)
        if (index != local && game.players[index].in_use != 0 &&
            game.players[index].status == OA_PLAYER_STATUS_COMPUTER) {
            computer = index;
            break;
        }
    require(computer != OA_PLAYER_COUNT, "the skirmish has no computer player");
    oa::Player& me = game.players[local];
    oa::Player& other = game.players[computer];
    auto* my_info = oa::world_player_info(&world, &me);
    auto* other_info = oa::world_player_info(&world, &other);
    require(my_info != nullptr && other_info != nullptr, "the players have no setup blocks");

    // What the check changes, put back at its end.
    const Extension saved_extension = extension_;
    const oa::ui::hud::TeamPanelHost saved_host = team_panel_host_;
    const oa::Player saved_me = me;
    const oa::Player saved_other = other;
    const PlayerSetupInfo saved_my_info = *my_info;
    const PlayerSetupInfo saved_other_info = *other_info;
    const auto saved_mapped = match_->sight().player_bits;
    const auto restore = [&] {
        if (match_paused_ && !match_finished_)
            resume_match_pause();
        extension_ = saved_extension;
        team_panel_host_ = saved_host;
        std::memcpy(me.alliance, saved_me.alliance, sizeof me.alliance);
        std::memcpy(me.allied_by, saved_me.allied_by, sizeof me.allied_by);
        std::memcpy(other.alliance, saved_other.alliance, sizeof other.alliance);
        std::memcpy(other.allied_by, saved_other.allied_by, sizeof other.allied_by);
        me.metal = saved_me.metal;
        me.team = saved_me.team;
        other.team = saved_other.team;
        other.status = saved_other.status;
        *my_info = saved_my_info;
        *other_info = saved_other_info;
        match_->follow_player_alliances(local);
        match_->follow_player_alliances(computer);
        match_->sight_mutable().player_bits = saved_mapped;
        game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags & ~console::kSimRunPaused);
    };

    ToldMachines told;
    team_panel_host_ = {};
    team_panel_host_.context = &told;
    team_panel_host_.alliance_changed = [](
                                            void* context, uint8_t from, uint8_t to, uint8_t allied
                                        ) {
        static_cast<ToldMachines*>(context)->log.push_back(
            "ally " + std::to_string(from) + ">" + std::to_string(to) + "=" + std::to_string(allied)
        );
    };
    team_panel_host_.setup_changed = [](void* context) {
        static_cast<ToldMachines*>(context)->log.emplace_back("setup");
    };
    team_panel_host_.remove_player = [](void* context, uint8_t player, uint8_t reason) {
        static_cast<ToldMachines*>(context)->log.push_back(
            "remove " + std::to_string(player) + " " + std::to_string(reason)
        );
    };
    team_panel_host_.game_changed = [](void* context) {
        static_cast<ToldMachines*>(context)->log.emplace_back("game");
    };
    team_panel_host_.resources_given =
        [](void* context, uint8_t from, uint8_t to, bool metal, float amount) {
            static_cast<ToldMachines*>(context)->log.push_back(
                std::string(metal ? "metal " : "energy ") + std::to_string(from) + ">" +
                std::to_string(to) + " " + std::to_string(static_cast<int>(amount))
            );
        };
    team_panel_host_.sight_shared = [](void* context, uint8_t from, uint8_t to) {
        static_cast<ToldMachines*>(context)->log.push_back(
            "sight " + std::to_string(from) + ">" + std::to_string(to)
        );
    };
    // The skirmish is taken as a multiplayer match shared with other machines.
    extension_.state = [](void*, const Runtime&) -> uint32_t {
        return extension_state::multiplayer | extension_state::shared_match;
    };

    bool running = true;
    const auto key = [&](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = code;
        event.key.scancode = scancode;
        handle_sdl_event(event, running);
    };
    // Clicks a control of the panel loaded as the match HUD where the pointer
    // would: `dx` and `dy` source pixels into it, or its centre for -1.
    const auto click = [&](std::string_view name, int dx = -1, int dy = -1) {
        require(match_hud_.has_value(), "no panel is loaded for " + std::string(name));
        const auto& gadgets = match_hud_->layout.gadgets;
        std::size_t index = 0;
        for (std::size_t i = 1; i < gadgets.size(); ++i)
            if (gadgets[i].common.name == name && gadgets[i].common.active != 0) {
                index = i;
                break;
            }
        require(index != 0, std::string(name) + " does not show on " + match_hud_panel_);
        const auto& common = gadgets[index].common;
        const int x = common.x + (dx < 0 ? common.width / 2 : dx);
        const int y = common.y + (dy < 0 ? common.height / 2 : dy);
        const auto at = oa::ui::display_layout::source_to_canvas(match_layout_, x, y);
        update_pointer(static_cast<float>(at.x), static_cast<float>(at.y));
        require(hovered_ == index, "the pointer over " + std::string(name) + " is not over it");
        activate_match_hud(index);
    };
    const auto shows = [&](std::string_view name) {
        if (!match_hud_)
            return false;
        for (const auto& gadget : match_hud_->layout.gadgets)
            if (gadget.common.name == name)
                return gadget.common.active != 0;
        return false;
    };
    const auto text_of = [&](std::string_view name) -> std::string {
        if (!match_hud_)
            return {};
        for (const auto& gadget : match_hud_->layout.gadgets) {
            if (gadget.common.name != name)
                continue;
            if (const auto* label = std::get_if<oa::ui::gui_layout::LabelFields>(&gadget.fields))
                return label->text;
            if (const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields))
                return button->text;
        }
        return {};
    };
    uint32_t clock_ms = 1000;
    const auto ticks_in_one_second = [&] {
        match_timing_.previous_clock =
            oa::base::game_loop::scaled_clock(clock_ms, match_clock_scale());
        match_timing_.remainder = 0.0F;
        const auto before = match_timing_.tick;
        for (uint32_t frame = 0; frame < kCheckFramesPerSecond; ++frame) {
            clock_ms += kCheckFrameMs;
            if (match_clock_steps())
                advance_match_clock(clock_ms);
        }
        return match_timing_.tick - before;
    };
    renderer::Surface frame;
    const auto band = match_layout_.top;
    const CanvasRect title{
        (match_layout_.width + match_layout_.left) / 2 - band,
        match_layout_.height / 2 - band / 2,
        2 * band,
        band
    };
    const auto title_pixels = [&] {
        render_match_surface();
        compose_match_frame(frame);
        return copy_rect(frame, title);
    };
    // The panels' frames go to the reports the navigation check writes.
    const auto snapshot = [&](const char* name) {
        render_match_surface();
        compose_match_frame(frame);
        write_ppm(fs::path("local/reports") / name, frame);
    };
    // The HUD layer as drawn now, and the match HUD panel's root on it.
    const auto hud_now = [&] {
        render_match_surface();
        return match_hud_cpu_;
    };
    const auto hud_root = [&] {
        require(match_hud_ && !match_hud_->layout.gadgets.empty(), "no panel is loaded");
        const auto& root = match_hud_->layout.gadgets.front().common;
        return oa::ui::display_layout::Rect{root.x, root.y, root.width, root.height};
    };
    // The pixels of `area` where the HUD layer drawn now differs from
    // `before`, darkened as the panel loader darkens the panel below a panel
    // it opens with shade_below when `darkened` says so; nothing darkens to
    // itself throughout.
    const auto differing_from =
        [&](renderer::Surface before, const oa::ui::display_layout::Rect& area, bool darkened) {
            if (darkened) {
                const auto undarkened = before;
                renderer::shade_panel_below(
                    before,
                    area.x,
                    area.y,
                    area.width,
                    area.height,
                    match_palette_,
                    display_.context.shade_table
                );
                require(
                    changed_pixels(
                        copy_rect(undarkened, {area.x, area.y, area.width, area.height}),
                        copy_rect(before, {area.x, area.y, area.width, area.height})
                    ) != 0,
                    "the shade table darkens nothing under " + match_hud_panel_
                );
            }
            const auto now = hud_now();
            return changed_pixels(
                copy_rect(before, {area.x, area.y, area.width, area.height}),
                copy_rect(now, {area.x, area.y, area.width, area.height})
            );
        };

    try {
        clear_local_selection();
        reset_match_command();
        pending_build_type_ = 0;
        apply_match_hud_for_selection();

        // The options menu of a shared match holds nothing and shows no
        // paused title.
        auto before = title_pixels();
        key(SDLK_F2, SDL_SCANCODE_F2);
        require(
            match_paused_ && match_hud_panel_ == oa::data::defs::gui_path("ARMOPT.GUI"),
            "F2 did not open ARMOPT.GUI"
        );
        // The menu opened over the running game, as it shows in the side column.
        const auto menu_root = hud_root();
        const auto menu_over_game = hud_now();
        const auto menu_title = changed_pixels(before, title_pixels());
        require(
            menu_title < kTextMinPixels,
            "the options menu of a shared match showed the paused title"
        );
        const auto menu_ticks = ticks_in_one_second();
        require(match_clock_steps() && menu_ticks > 0, "the options menu held a shared match");
        // The preferences it opens stay on the match screen, PREFS.GUI in
        // the side column, and the match goes on beneath them and beneath a
        // tab's sub-panel. Escape takes them back to the menu.
        click("PREFS");
        require(
            screen_ == Screen::match && match_paused_ &&
                match_hud_panel_ == oa::data::defs::gui_path("PREFS.GUI"),
            "PREFS did not open PREFS.GUI over the match"
        );
        const auto preference_ticks = ticks_in_one_second();
        require(
            match_running() && match_clock_steps() && preference_ticks > 0,
            "the preferences held a shared match"
        );
        click("SOUND");
        require(shows("FXVOL"), "SOUND did not open SOUNDSRT.GUI");
        require(ticks_in_one_second() > 0, "the preferences' SOUND held a shared match");
        // A speed another machine sets while they are open stays: a click
        // that sets no speed writes none.
        const auto opened_requested = game.requested_speed;
        const auto opened_current = game.current_speed;
        const auto opened_rates =
            std::pair{match_timing_.requested_rate, match_timing_.actual_rate};
        const auto opened_preferences = preferences_;
        const auto other_speed = static_cast<uint16_t>(opened_requested == 15 ? 14 : 15);
        game.requested_speed = other_speed;
        game.current_speed = other_speed;
        click("SOUND");
        require(
            game.requested_speed == other_speed && game.current_speed == other_speed &&
                preferences_.current_game_speed == other_speed,
            "a click in the preferences put back the speed they opened with"
        );
        key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
        require(
            game.requested_speed == other_speed,
            "leaving the preferences put back the speed they opened with"
        );
        game.requested_speed = opened_requested;
        game.current_speed = opened_current;
        match_timing_.requested_rate = opened_rates.first;
        match_timing_.actual_rate = opened_rates.second;
        preferences_.game_speed = opened_preferences.game_speed;
        preferences_.current_game_speed = opened_preferences.current_game_speed;
        flush_preferences();
        require(
            screen_ == Screen::match && match_paused_ &&
                match_hud_panel_ == oa::data::defs::gui_path("ARMOPT.GUI"),
            "leaving the preferences did not return to ARMOPT.GUI"
        );
        key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
        require(!match_paused_, "Escape did not close ARMOPT.GUI");

        // A pause another machine sets holds the clock and shows the title.
        before = title_pixels();
        game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags | console::kSimRunPaused);
        require(
            match_clock_steps() && ticks_in_one_second() == 0,
            "a pause set elsewhere did not hold the clock"
        );
        const auto pause_title = changed_pixels(before, title_pixels());
        require(pause_title >= kTextMinPixels, "a pause set elsewhere showed no paused title");
        key(SDLK_F2, SDL_SCANCODE_F2);
        key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
        require(
            !match_paused_ && (game.sim_run_flags & console::kSimRunPaused) != 0 &&
                ticks_in_one_second() == 0,
            "closing the shared game's menu cleared its synchronized pause"
        );
        key(SDLK_TAB, SDL_SCANCODE_TAB);
        key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
        require(
            !match_paused_ && (game.sim_run_flags & console::kSimRunPaused) != 0 &&
                ticks_in_one_second() == 0,
            "closing the team menu cleared the synchronized pause"
        );
        game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags & ~console::kSimRunPaused);
        require(ticks_in_one_second() > 0, "the match did not resume when the pause bit cleared");

        // Tab opens the tab menu over the running match; ALLIES and SHARE
        // show, CONTROL only for the host. The side column's panel, a local
        // unit's, stays under it, darkened.
        for (const auto& slot : match_->world().slots)
            if (slot.unit_index != 0 && slot.unit != nullptr && slot.owner_index == local) {
                adopt_selection(slot.unit_index);
                break;
            }
        require(selected_match_unit_ != 0, "no local unit to select");
        apply_match_hud_for_selection();
        const auto side_root = hud_root();
        const auto side_panel = hud_now();
        key(SDLK_TAB, SDL_SCANCODE_TAB);
        require(
            team_panel_open() && match_hud_panel_ == oa::data::defs::gui_path("TABMENU.GUI"),
            "Tab did not open TABMENU.GUI"
        );
        require(
            differing_from(side_panel, side_root, true) == 0,
            "the side column's panel is not darkened under the tab menu"
        );
        require(
            shows("ALLIES") && shows("SHARE") && !shows("CONTROL"),
            "the tab menu's buttons are wrong"
        );
        snapshot("native-team-tabmenu.ppm");
        require(ticks_in_one_second() > 0, "the tab menu held the match");
        key(SDLK_TAB, SDL_SCANCODE_TAB);
        require(!team_panel_open() && !match_paused_, "Tab did not close the tab menu");

        // Over a match played on this machine alone, as a skirmish under
        // teams.alliance-menu-all-game-types, the tab menu holds nothing and
        // shows no paused title: 3.1c holds such a game only for its in-game
        // menu.
        extension_.state = [](void*, const Runtime&) -> uint32_t {
            return extension_state::multiplayer;
        };
        const auto alone_title = title_pixels();
        key(SDLK_TAB, SDL_SCANCODE_TAB);
        require(
            team_panel_open() && match_hud_panel_ == oa::data::defs::gui_path("TABMENU.GUI"),
            "Tab did not open TABMENU.GUI over a match played alone"
        );
        require(
            changed_pixels(alone_title, title_pixels()) == 0,
            "the tab menu showed the paused title over a match played alone"
        );
        require(ticks_in_one_second() > 0, "the tab menu held a match played alone");
        key(SDLK_TAB, SDL_SCANCODE_TAB);
        require(!team_panel_open() && !match_paused_, "Tab did not close the tab menu");
        extension_.state = [](void*, const Runtime&) -> uint32_t {
            return extension_state::multiplayer | extension_state::shared_match;
        };

        // The tab menu's OPTIONS opens the in-game menu over it: the menu
        // stays under it, darkened, and the menu covers the side column's
        // panel as it does opened over the running game.
        key(SDLK_TAB, SDL_SCANCODE_TAB);
        const auto tab_root = hud_root();
        const auto tab_menu = hud_now();
        click("OPTIONS");
        require(
            match_paused_ && match_hud_panel_ == oa::data::defs::gui_path("ARMOPT.GUI"),
            "the tab menu's OPTIONS did not open ARMOPT.GUI"
        );
        require(
            differing_from(tab_menu, tab_root, true) == 0,
            "the tab menu is not darkened under ARMOPT.GUI"
        );
        require(
            differing_from(menu_over_game, menu_root, false) == 0,
            "ARMOPT.GUI opened from the tab menu does not cover the side column's panel"
        );
        key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
        require(
            !team_panel_open() && !match_paused_,
            "Escape did not close ARMOPT.GUI opened from the tab menu"
        );
        clear_local_selection();
        apply_match_hud_for_selection();

        // 'h' opens SHARE.GUI; OK gives the slider's metal and a chosen unit
        // to the computer player and shares the map.
        const auto kbot = oa::sim::unit_spawn::find_type_index(
            spawn_type_names_, match_side_prefix() == "cor" ? "CORAK" : "ARMPW"
        );
        require(kbot != 0, "the side's kbot is missing");
        uint16_t anchor = 0;
        for (const auto& slot : match_->world().slots)
            if (slot.unit_index != 0 && slot.unit != nullptr && slot.owner_index == local) {
                anchor = slot.unit_index;
                break;
            }
        require(anchor != 0, "no local unit");
        const auto& anchor_unit = *match_->world().slots[anchor].unit;
        const auto x = static_cast<int32_t>(anchor_unit.position[0] >> 16) + kGiftSpread;
        const auto z = static_cast<int32_t>(anchor_unit.position[2] >> 16);
        oa::sim::unit_spawn::Request request;
        request.player = local;
        request.type = kbot;
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>(x) << 16,
            static_cast<uint32_t>(
                match_->map_height(static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16)
            ) << 16,
            static_cast<uint32_t>(z) << 16
        };
        auto* gift = match_->create(request);
        require(gift != nullptr && gift->unit != nullptr, "could not add a kbot to give");
        const auto count_kbots = [&](uint8_t owner) {
            int count = 0;
            for (const auto& slot : match_->world().slots)
                if (slot.unit_index != 0 && slot.unit != nullptr && slot.owner_index == owner &&
                    slot.unit->type_index == kbot &&
                    (slot.unit->flags & OA_UNIT_FLAG_DEATH_PENDING) == 0)
                    ++count;
            return count;
        };
        const auto their_kbots = count_kbots(computer);
        clear_local_selection();
        adopt_selection(gift->unit_index);
        me.metal = kGiftMetal;
        key(SDLK_H, SDL_SCANCODE_H);
        require(
            team_panel_open() && match_hud_panel_ == oa::data::defs::gui_path("SHARE.GUI"),
            "'h' did not open SHARE.GUI"
        );
        require(ticks_in_one_second() > 0, "SHARE.GUI held the match");
        click("PLYRLIST", 4, 3);
        const auto* metal_slider = [&]() -> const oa::ui::gui_layout::Gadget* {
            for (const auto& gadget : match_hud_->layout.gadgets)
                if (gadget.common.name == "METAL")
                    return &gadget;
            return nullptr;
        }();
        require(metal_slider != nullptr, "SHARE.GUI has no METAL slider");
        // The knob dragged to the end of the bar gives the whole store.
        drag_check_knob("METAL", metal_slider->common.width);
        const auto given = static_cast<int>(kGiftMetal);
        require(text_of("METAL#") == std::to_string(given), "METAL# reads " + text_of("METAL#"));
        click("SHARUNIT");
        click("MAPINFO");
        snapshot("native-team-share.ppm");
        const auto metal_before = me.metal;
        // Cells the local player has mapped that the computer player has not;
        // the first few rows are made so.
        const auto mine = static_cast<uint16_t>(1U << local);
        const auto theirs = static_cast<uint16_t>(1U << computer);
        const auto unshared_cells = [&] {
            uint32_t count = 0;
            for (const auto bits : match_->sight().player_bits)
                count += (bits & mine) != 0 && (bits & theirs) == 0 ? 1U : 0U;
            return count;
        };
        auto& mapped = match_->sight_mutable().player_bits;
        for (std::size_t cell = 0; cell < std::min<std::size_t>(mapped.size(), kMappedCells);
             ++cell)
            mapped[cell] = static_cast<uint16_t>((mapped[cell] | mine) & ~theirs);
        const auto unshared_before = unshared_cells();
        click("OK");
        require(!team_panel_open() && !match_paused_, "OK did not close SHARE.GUI");
        const auto gave = "metal " + std::to_string(local) + ">" + std::to_string(computer) + " " +
                          std::to_string(given);
        const auto shared = "sight " + std::to_string(local) + ">" + std::to_string(computer);
        require(
            told.log == std::vector<std::string>{gave, shared}, "the share was not told as " + gave
        );
        require(
            me.metal == metal_before - kGiftMetal, "the given metal stayed with the local player"
        );
        require(
            count_kbots(computer) == their_kbots + 1,
            "the chosen kbot did not go to the computer player"
        );
        require(
            unshared_before > 0 && unshared_cells() == 0,
            "MAPINFO did not give the computer player the local player's map (" +
                std::to_string(unshared_before) + " cells before, " +
                std::to_string(unshared_cells()) + " after)"
        );

        // The computer player is taken as another machine's human for
        // ALLIES.GUI, where only such players have a toggle, and on no team
        // with the local player, whose team mates' toggles are greyed.
        other.status = OA_PLAYER_STATUS_MIRRORED;
        other_info->state = kHumanPlaying;
        me.team = OA_PLAYER_NO_TEAM;
        other.team = OA_PLAYER_NO_TEAM;
        told.log.clear();
        key(SDLK_TAB, SDL_SCANCODE_TAB);
        click("ALLIES");
        require(
            team_panel_open() && match_hud_panel_ == oa::data::defs::gui_path("ALLIES.GUI"),
            "ALLIES did not open ALLIES.GUI"
        );
        const auto toggle = "LIVEALLY" + std::to_string(computer);
        const auto row = "LIVEPLYR" + std::to_string(computer);
        require(
            shows(row) && text_of(row) == player_name(other),
            "ALLIES.GUI has no row for the other player"
        );
        const bool was_allied = me.alliance[computer] != 0;
        click(toggle);
        snapshot("native-team-allies.ppm");
        require(
            (me.alliance[computer] != 0) != was_allied, "the toggle did not change the alliance"
        );
        require(
            match_->allied(local, computer) != was_allied,
            "the simulation's alliance did not follow the toggle"
        );
        const auto allied = "ally " + std::to_string(local) + ">" + std::to_string(computer) + "=" +
                            std::to_string(was_allied ? 0 : 1);
        require(
            told.log == std::vector<std::string>{allied}, "the alliance was not told as " + allied
        );
        const auto lines = match_message_lines();
        const auto said =
            (was_allied ? "broke alliance with " : "allied with ") + player_name(other);
        require(
            !lines.empty() && lines.back().find(said) != std::string::npos,
            "the alliance was not said in chat"
        );
        click("OK");
        require(!team_panel_open(), "OK did not close ALLIES.GUI");

        // CONTROL.GUI shows for the host; WATCHING and a removal tell the others.
        my_info->role = static_cast<uint8_t>(my_info->role | kHostRole);
        told.log.clear();
        key(SDLK_TAB, SDL_SCANCODE_TAB);
        require(shows("CONTROL"), "CONTROL does not show for the host");
        const auto control_tab_root = hud_root();
        const auto control_tab_menu = hud_now();
        click("CONTROL");
        require(
            team_panel_open() && match_hud_panel_ == oa::data::defs::gui_path("CONTROL.GUI"),
            "CONTROL did not open CONTROL.GUI"
        );
        // The tab menu stays under it, darkened.
        require(
            differing_from(control_tab_menu, control_tab_root, true) == 0,
            "the tab menu is not darkened under CONTROL.GUI"
        );
        require(!shows("LIVEPLYR" + std::to_string(local)), "CONTROL.GUI shows the local player");
        const auto control_panels_below = match_panels_below_.size();
        const auto watching = (my_info->options & OA_SETUP_OPTION_WATCHING_ALLOWED) != 0;
        click("WATCHING");
        snapshot("native-team-control.ppm");
        require(
            ((my_info->options & OA_SETUP_OPTION_WATCHING_ALLOWED) != 0) != watching,
            "WATCHING did not change watching"
        );
        click(row);
        require(
            match_hud_panel_ == oa::data::defs::gui_path("YESORNO.GUI"),
            "a player's row did not ask to remove it"
        );
        require(
            text_of("TITLE").find(player_name(other)) != std::string::npos,
            "the removal question does not name the player"
        );
        snapshot("native-team-removal.ppm");
        // The question takes its quick keys, as the gadget engine gives them
        // from its captions: 'n' answers No and 'y' Yes.
        key(SDLK_N, SDL_SCANCODE_N);
        require(
            team_panel_open() && match_hud_panel_ == oa::data::defs::gui_path("CONTROL.GUI") &&
                match_paused_ && match_panels_keyboard_,
            "'n' did not return to CONTROL.GUI"
        );
        require(told.log == std::vector<std::string>{"setup"}, "'n' removed a player");
        click(row);
        require(
            match_hud_panel_ == oa::data::defs::gui_path("YESORNO.GUI"),
            "a player's row did not ask again to remove it"
        );
        key(SDLK_Y, SDL_SCANCODE_Y);
        require(
            team_panel_open() && match_hud_panel_ == oa::data::defs::gui_path("CONTROL.GUI") &&
                match_paused_ && match_panels_keyboard_,
            "'y' did not return to CONTROL.GUI"
        );
        // It comes back over the panels it was opened over, and no more.
        require(
            match_panels_below_.size() == control_panels_below,
            "CONTROL.GUI came back from the removal question over other panels"
        );
        click("OK");
        require(!team_panel_open(), "OK did not close CONTROL.GUI");
        const auto removed = "remove " + std::to_string(computer) + " 1";
        require(
            told.log == std::vector<std::string>{"setup", removed, "game"},
            "CONTROL.GUI told the others the wrong things"
        );
    } catch (...) {
        restore();
        throw;
    }
    restore();
    std::cout << "team panel check: a shared match runs under its menus and preferences and "
                 "holds on a pause set elsewhere; TABMENU, SHARE (map shared here), ALLIES "
                 "(followed by the simulation), CONTROL and the removal question tell the other "
                 "machines\n";
}

} // namespace oa::app
