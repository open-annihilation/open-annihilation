// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Loading screen and entering/leaving a match.
#include "oa/app/runtime.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/ui/decoded.hpp"
#include "oa/app/asset_files.hpp"
#include "oa/data/campaign/campaign_file.hpp"
#include "oa/sim/scenario/commander_rules.hpp"
#include "oa/sim/selection.hpp"
#include "oa/data/defs/palette.hpp"
#include "oa/ui/frontend_renderer/gadget_draw.hpp"
#include "oa/present/blit.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/present/pcx.hpp"
#include "oa/present/raster.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <tuple>

namespace oa::app {
namespace {

bool read_image_palette(void* context, const char* path, uint8_t* palette) {
    try {
        const auto image = oa::ui::decoded::require(
            oa::decode_pcx(static_cast<const oa::AssetStore*>(context)->read(path).bytes), path
        );
        if (!image.palette)
            return false;
        std::copy(image.palette->begin(), image.palette->end(), palette);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

oa::PaletteBytes load_active_palette(const oa::AssetStore& assets) {
    const oa::data::defs::Files files = asset_files(assets);
    oa::PaletteBytes palette{};
    static_assert(palette.size() == oa::data::defs::palette_file_bytes);
    if (!oa::data::defs::load_palette_file(
            &files,
            "PALETTE",
            {const_cast<oa::AssetStore*>(&assets), read_image_palette},
            palette.data()
        ))
        throw std::runtime_error("palettes/PALETTE.PAL");
    return palette;
}

void Runtime::ensure_ui_colors() {
    if (ui_colors_ready_)
        return;
    try {
        loading_palette_ = load_active_palette(assets_);
        const auto gui = assets_.read("palettes/guipal.pal").bytes;
        oa::PaletteBytes gui_palette{};
        if (gui.size() != gui_palette.size())
            throw std::runtime_error("palette has invalid size");
        std::copy(gui.begin(), gui.end(), gui_palette.begin());
        ui_colors_ = oa::remap_palette(gui_palette, loading_palette_);
        ui_colors_ready_ = true;
    } catch (const std::exception& error) {
        std::cerr << "ui palettes unavailable: " << error.what() << '\n';
    }
}

void Runtime::ensure_loading_screen() {
    if (loading_background_.surface.pixels != nullptr)
        return;
    ensure_ui_colors();
    try {
        const auto bytes = assets_.read("bitmaps/Loadgame2bg.pcx").bytes;
        oa::present::MemoryReader reader{bytes};
        auto stream = oa::present::memory_reader_stream(reader);
        const auto status = oa::present::load_pcx_surface(stream, loading_background_, nullptr);
        if (status != oa::present::PcxStatus::ok) {
            loading_background_ = {};
            std::cerr << "loadgame2bg unavailable: " << oa::present::pcx_status_text(status)
                      << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "loadgame2bg unavailable: " << error.what() << '\n';
    }
    const auto load_gaf = [this](const char* path, oa::present::GafSprites& gaf) {
        try {
            const auto status = oa::present::relocate_gaf(assets_.read(path).bytes, gaf);
            if (status != oa::present::GafStatus::ok) {
                gaf = {};
                std::cerr << path << " unavailable: " << oa::present::gaf_status_text(status)
                          << '\n';
            }
        } catch (const std::exception& error) {
            std::cerr << path << " unavailable: " << error.what() << '\n';
        }
    };
    ensure_gui_font();
    load_gaf("anims/commongui.gaf", loading_gui_);
    loading_lightbar_ =
        oa::present::gaf_frame(oa::present::find_gaf_sequence(loading_gui_, "LIGHTBAR"), 0);
}

void Runtime::begin_loading_screen() {
    load_progress_.fill(0);
    loading_flash_.fill(0);
    screen_ = Screen::loading;
    apply_output_mode();
    ensure_loading_screen();
    enter_loading_display();
    pump_loading_screen();
}

void Runtime::set_load_progress(std::size_t row, uint8_t percent) {
    if (row < load_progress_.size()) {
        const auto clamped = percent > 100 ? uint8_t{100} : percent;
        if (clamped == 100 && load_progress_[row] != 100)
            loading_flash_[row] = 0x1e;
        load_progress_[row] = clamped;
    }
    // The world is built without frames; the extension keeps its own work
    // going here.
    call_hook_or_report<&Extension::load_progress>(
        extension_, hook_error_report(), *this, load_progress_.data(), load_progress_.size()
    );
    pump_loading_screen();
}

void Runtime::pump_loading_screen() {
    if (sdl_.window == nullptr && !options_.headless_check)
        return;
    screen_ = Screen::loading;
    if (options_.headless_check) {
        draw_loading_screen();
        return;
    }
    render();
    SDL_PumpEvents();
    SDL_Event event;
    // A quit asked through ScreenServices::quit or the automation endpoint
    // stops the load as the window's close button does; the run then ends.
    if (quit_requested_)
        throw std::runtime_error("loading cancelled");
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            throw std::runtime_error("loading cancelled");
        // A reset or lost device during the load is handled as in the
        // game, so that the match's first frame finds working textures.
        if (take_render_event(event))
            continue;
        // Alt+Enter switches full screen while a match loads too. The
        // loading screen takes no other input, so whether it was Alt+Enter
        // does not matter.
        std::ignore = take_full_screen_event(event);
    }
}

void Runtime::enter_loading_display() {
    auto& display = display_.context;
    const auto palette = oa::present::palette_from_bytes(loading_palette_);
    oa::present::apply_palette_entries(
        display, palette.entries, 0, OA_PALETTE_COLORS, display.device_palette
    );
    oa::present::use_standard_offscreen(display, display_.offscreen);
}

namespace {

// Offsets into the guipal-derived colour table used for the progress chips.
constexpr std::size_t kLoadChipDoneColor = 10;
constexpr std::size_t kLoadChipBusyColor = 12;
constexpr int32_t kLoadLabelX = 0x5a;
constexpr int32_t kLoadChipLeft = 0xcd;
constexpr int32_t kLoadChipHeight = 0x14;
constexpr uint8_t kLoadFlashStep = 2;

} // namespace

void Runtime::draw_loading_screen() {
    namespace present = oa::present;
    present::set_active_surface(&display_.offscreen.surface);
    oa::Surface target{};
    if (present::lock_display_surface(target) == 0)
        return;
    if (loading_background_.surface.pixels != nullptr)
        present::blit_surface(&target, &loading_background_.surface, 0, 0);
    const auto* font = gui_font_.sequences.empty() ? nullptr : &gui_font_;
    static constexpr std::array<const char*, 6> labels{
        "Textures", "Terrain", "Units", "Animation", "3D Data", "Explosions"
    };
    static constexpr std::array<int32_t, 6> bar_y{0x87, 0xb1, 0xda, 0x106, 0x130, 0x15b};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (loading_flash_[i] > 1)
            loading_flash_[i] = static_cast<uint8_t>(loading_flash_[i] - kLoadFlashStep);
        else
            loading_flash_[i] = 0;
        const auto percent = static_cast<int32_t>(load_progress_[i]);
        const auto chip_color =
            ui_colors_[percent >= 100 ? kLoadChipDoneColor : kLoadChipBusyColor];
        present::set_text_colors(chip_color, static_cast<int32_t>(present::text_transparent()));
        // The loading screen keeps the game's own fonts whatever the
        // Language settings say: a character a font lacks is drawn
        // in the modern fonts, as with them off. The label is in the game's
        // language, as gamedata\translate.tdf gives it.
        const char* translated = game_translation(labels[i]);
        renderer::draw_gadget_text(
            &target,
            font,
            translated != nullptr ? translated : labels[i],
            kLoadLabelX,
            bar_y[i],
            renderer::gadget_text_unbounded,
            loading_flash_[i],
            false
        );
        const oa::Rect32 chip{
            kLoadChipLeft, bar_y[i], kLoadChipLeft + (percent * 7) / 2, bar_y[i] + kLoadChipHeight
        };
        present::fill_clipped_rect(&target, chip, chip_color);
        // LIGHTBAR is chrome with holes over the chips; its hotspot is
        // zeroed before every draw.
        if (loading_lightbar_ != nullptr) {
            loading_lightbar_->origin_x = 0;
            loading_lightbar_->origin_y = 0;
            present::draw_sprite(&target, loading_lightbar_, kLoadChipLeft, bar_y[i]);
        }
    }
    call_hook_or_report<&Extension::draw_loading>(
        extension_, hook_error_report(), *this, target, font
    );
    present::unlock_display_surface();
    show_display_frame();
}

void Runtime::teardown_match() {
    // Director mode holds the match's sound hooks and camera paths; it
    // gives them back before the match goes.
    leave_director_mode();
    call_hook_or_report<&Extension::match_event>(
        extension_, hook_error_report(), *this, MatchEvent::torn_down
    );
    // A team panel open as the match ended goes with it, and so do the
    // preferences its in-game menu opened.
    forget_team_panel();
    forget_match_preferences();
    // A watcher's switched view and what other machines reported go with
    // the match; the resource panel keeps its place.
    watched_player_ = OA_PLAYER_COUNT;
    watched_sight_ = WatchedSight::game;
    resource_panel_.viewed_slot = 0;
    resource_panel_.locked_slot = 0;
    resource_panel_.dragging = false;
    shared_views_ = {};
    whiteboard_ = {};
    whiteboard_input_ = {};
    megamap_open_ = false;
    megamap_ = {};
    offline_effects_.unbind();
    offline_services_.clear_match();
    offline_services_.set_on_screen_test(nullptr, nullptr);
    effect_boundary_.clock = nullptr;
    live_renderer_.reset();
    reset_match_effect_sources();
    match_.reset();
    release_match_mutators();
    // A replay or a saved game puts the player's mutators back once its match
    // ends. A teardown before that match starts leaves the held list as it is.
    finish_loadout_once();
    // The menus keep the sides the game started with.
    restore_run_side_table();
    release_unsaved_unit_limit();
    // A later match may be built at the same address on other game data:
    // the side column's pages are measured again for it.
    side_column_measured_for_ = nullptr;
    game_speed_lock_.reset();
    end_render_tier_match();
    unit_playout_.reset();
    // Its models and radar are keyed by address; a later match can reuse the
    // addresses, and so can the sprite pages' frames.
    match_models_.reset();
    // The explosion frames rendered for its draws go too; the files they
    // were read from stay checked for the next match.
    explosion_frame_cache_.reset();
    free_full_match_state();
    // Each 3D feature's cached image and silhouette go with the match's
    // renderer, as they did when the renderer kept them.
    for (auto& feature : match_features_)
        feature.state = {};
    radar_state_.release();
    selected_match_unit_ = 0;
    hovered_match_unit_ = 0;
    on_screen_units_.clear();
    unit_info_panel_.reset();
    stop_match_tracking();
    match_hud_.reset();
    // A GAF this match could not read is tried again in the next match.
    missing_gaf_paths_.clear();
    match_tick_blocked_ = false;
    // The rules the player's overrides changed while it ran apply from here,
    // so that the next match is built with them.
    play_latest_profile();
    // A match that ended deletes the crash record. A fatal unwind never
    // reaches here, so the file stays for the next start.
    clear_mutator_sentinel();
}

void Runtime::leave_match() {
    if (match_)
        call_hook_or_report<&Extension::match_event>(
            extension_, hook_error_report(), *this, MatchEvent::left
        );
    match_paused_ = false;
    match_panels_keyboard_ = false;
    match_finished_ = false;
    // A stage belongs to the match it set up.
    stage_.reset();
    outcome_over_menu_ = false;
    match_outcome_ = sim::scenario::Outcome::ongoing;
    campaign_mission_ = false;
    stop_match_tracking();
    radar_explored_.clear();
    unit_info_panel_.reset();
    chat_composing_ = false;
    chat_buffer_.clear();
    chat_composition_.clear();
    match_zoom_ = kDefaultBattlefieldZoom;
    match_zoom_target_ = kDefaultBattlefieldZoom;
    zoom_focus_ = {};
    exact_view_ = {};
    view_hold_ = {};
    zoom_wheel_ = {};
    zoom_clock_valid_ = false;
    terrain_cache_cam_x_ = kUncachedTerrainCamera;
    terrain_cache_zoom_ = -1.0F;
    match_terrain_cache_ = {};
    far_terrain_ = {};
    match_scene_cpu_ = {};
    match_hud_cpu_ = {};
    match_world_cpu_ = {};
    destroy_match_layer_textures();
    free_accelerated_match_textures();
    teardown_match();
    start_menu_music();
    // The match that used a pack map's files is over: they are unmounted.
    release_pack_map();
}

void Runtime::enter_match_view() {
    // Mission start sets the session's cheat flag.
    if (match_)
        session_cheats_allowed_ = sim::scenario::session_cheats_allowed(
            match_session_kind(), match_->state(), session_cheats_allowed_
        );
    stop_menu_music();
    match_palette_ = load_active_palette(assets_);
    if (!texture_catalog_)
        texture_catalog_ = oa::present::world_renderer::load_texture_catalog(assets_);
    if (match_fx_.sequences.empty())
        append_gaf_file(match_fx_, "anims/FX.GAF");
    if (match_fog_.sequences.empty())
        append_gaf_file(match_fog_, "anims/FOG.GAF");
    load_side_hud();
    load_match_chrome();
    screen_ = Screen::match;
    selected_ = -1;
    hovered_.reset();
    match_command_ = MatchCommand::none;
    // No building site has been tested under the pointer yet.
    if (match_) {
        auto& game = match_->state().game;
        oa::sim::gameplay_input::set_pointer_flags(
            game,
            static_cast<uint8_t>(
                oa::sim::gameplay_input::pointer_flags(game) &
                ~oa::sim::gameplay_input::pointer_build_site_clear
            )
        );
    }
    apply_output_mode();
    // With --trace-input, the window and the pointer as the match begins.
    trace_window("match");
    select_local_commander();
    apply_match_hud_for_selection();
    render_match_surface();
    touch_match_started();
}

void Runtime::select_local_commander() {
    if (!match_)
        return;
    for (auto& slot : match_->world().slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr ||
            slot.owner_index != match_local_player_ || !slot.unit->type_index)
            continue;
        clear_local_selection();
        slot.unit->flags |= OA_UNIT_FLAG_SELECTED;
        match_->selection().panel_unit_id = 0;
        match_->selection().frame_flags |= oa::sim::selection::frame_flag_selection_changed;
        offline_services_.refresh_selected_unit(slot);
        selected_match_unit_ = slot.unit_index;
        status_ = "commander selected";
        return;
    }
}

} // namespace oa::app
