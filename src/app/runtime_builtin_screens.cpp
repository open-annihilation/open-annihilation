// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Built-in frontend screens, dispatcher steps and the services table handed to
// registered screen packages.
#include "oa/app/runtime.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/mod_profile.hpp"
#include "engine_settings_state.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend/main_menu.hpp"
#include "oa/ui/campaign/endgame.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

struct BuiltinScreens {
    static Runtime& host(ScreenContext* ctx) { return *static_cast<Runtime*>(ctx->host); }

    // Services.
    static void request_screen(void* host, ScreenId id) {
        static_cast<Runtime*>(host)->pending_screen_ = id;
    }

    static void play_sound(void* host, const char* name) {
        auto& runtime = *static_cast<Runtime*>(host);
        if (name == nullptr)
            return;
        if (runtime.audio_registry_.find(name) != oa::audio::game_audio::missing_sound) {
            runtime.play_ui_sound(name, 0);
            return;
        }
        if (runtime.options_.mute)
            return;
        const std::string resource = runtime.screen_sound_resource(name);
        if (runtime.sound_found_missing(resource))
            return;
        std::string error;
        if (!runtime.audio_player_.play_resource(resource, error))
            runtime.report_unplayed_sound(resource, error);
    }

    static void register_sound(void* host, const char* category, const char* file) {
        if (category != nullptr && file != nullptr)
            static_cast<Runtime*>(host)->audio_registry_.add(category, file);
    }

    static int read_number(void* host, const char* section, const char* key, uint32_t* value) {
        const auto found = static_cast<Runtime*>(host)->read_number(section, key);
        if (!found)
            return 0;
        *value = *found;
        return 1;
    }

    static void write_number(void* host, const char* section, const char* key, uint32_t value) {
        static_cast<Runtime*>(host)->write_number(section, key, value);
    }

    static int
    read_string(void* host, const char* section, const char* key, char* out, std::size_t capacity) {
        if (capacity == 0)
            return 0;
        const auto found = static_cast<Runtime*>(host)->read_string(section, key, capacity);
        if (!found)
            return 0;
        std::memcpy(out, found->c_str(), found->size() + 1U);
        return 1;
    }

    static void write_string(void* host, const char* section, const char* key, const char* value) {
        static_cast<Runtime*>(host)->write_string(section, key, value);
    }

    static void set_status(void* host, const char* text) {
        static_cast<Runtime*>(host)->status_ = text != nullptr ? text : "";
    }

    static void set_frontend_signal(void* host, uint8_t signal) {
        auto& state = static_cast<Runtime*>(host)->state_;
        state.signal = signal;
        state.pending_signal = signal;
    }

    static void set_frontend_state(void* host, uint8_t state) {
        static_cast<Runtime*>(host)->state_.state = state;
    }

    // The main menu is not held as a gadget panel: popping it drops its
    // gadgets, so nothing on it can be hovered or pressed until it reloads.
    static void close_main_menu_panel(void* host) {
        auto& runtime = *static_cast<Runtime*>(host);
        runtime.resources_.layout.gadgets.clear();
        runtime.selected_ = -1;
        runtime.hovered_.reset();
        runtime.menu_sparks_ = {};
    }

    static void reload_main_menu_panel(void* host) {
        auto& runtime = *static_cast<Runtime*>(host);
        runtime.resources_ = load_main_menu(runtime);
        setup_main_menu_panel(runtime);
        runtime.selected_ = -1;
        runtime.hovered_.reset();
    }

    // The match chat line keeps text input while it is open. The screens
    // name no text box when they turn text input on, so no field's place
    // goes with it and the system places an on-screen keyboard as it would.
    static void set_text_input(void* host, int enabled) {
        auto& runtime = *static_cast<Runtime*>(host);
        if (runtime.sdl_.window == nullptr)
            return;
        if (enabled != 0)
            runtime.start_text_input(std::nullopt);
        else if (!runtime.chat_composing_)
            runtime.stop_text_input();
    }

    static uint32_t current_tick(void* host) {
        return static_cast<Runtime*>(host)->frontend_tick();
    }

    static void pointer_position(void* host, int32_t* x, int32_t* y) {
        const auto* runtime = static_cast<Runtime*>(host);
        *x = static_cast<int32_t>(runtime->pointer_x_);
        *y = static_cast<int32_t>(runtime->pointer_y_);
    }

    static void set_main_menu_overlay(void* host, int present) {
        static_cast<Runtime*>(host)->main_menu_overlay_ = present != 0;
    }

    // The run ends once the event or frame that asked has been handled
    // (finish_quit_request), so no callback still running meets a match
    // that has gone; run() returns the status once the loop ends.
    static void quit(void* host, const char* reason, int exit_code) {
        auto& runtime = *static_cast<Runtime*>(host);
        runtime.quit_requested_ = true;
        runtime.quit_reason_ = reason != nullptr ? reason : "";
        runtime.exit_status_ = exit_code;
    }

    // The CD and track music go on.
    static void stop_sounds(void* host) {
        auto& runtime = *static_cast<Runtime*>(host);
        runtime.audio_player_.stop_all();
        runtime.menu_music_playing_ = false;
    }

    static void play_sound_alternate(void* host, const char* name) {
        // A sound that does not start is reported by play_alternate_sound.
        if (name != nullptr)
            std::ignore = static_cast<Runtime*>(host)->play_alternate_sound(name);
    }

    // The pass runs from apply_screen_request, never inside a callback.
    static void run_frontend(void* host) {
        static_cast<Runtime*>(host)->frontend_pass_requested_ = true;
    }

    static renderer::MainMenuResources load_main_menu(Runtime& runtime) {
        return renderer::load_main_menu(
            runtime.assets_,
            runtime.main_menu_overlay_ ? renderer::MainMenuLayout::with_overlay
                                       : renderer::MainMenuLayout::base_game
        );
    }

    // Dialog host.
    static void dialog_sound(void* host, const char* name) {
        static_cast<Runtime*>(host)->play_ui_sound(name != nullptr ? name : "", 0);
    }

    // The end screen holds state 8 while CDCHECK.GUI is up; the click
    // handler returns state 5 once the disc is present, which builds the
    // outcome screen.
    static int32_t dialog_cd_check_click(void* host, const char* control) {
        auto& runtime = *static_cast<Runtime*>(host);
        oa::ui::campaign::FrontendHost campaign_host{};
        campaign_host.context = &runtime;
        campaign_host.translate = Runtime::translation_hook;
        campaign_host.play_sound = dialog_sound;
        campaign_host.disc_present = [](void* context) {
            return static_cast<Runtime*>(context)->find_disc(menu::Disc::campaign) != 0;
        };
        campaign_host.message_box = [](void* context, const char* text, int32_t width) {
            static_cast<Runtime*>(context)->show_frontend_message(
                text != nullptr ? text : "", width, entry::message_show_ok, entry::message_fit_width
            );
        };
        constexpr int32_t waiting_for_disc = 8;
        if (oa::ui::campaign::cd_check_click(&campaign_host, control, waiting_for_disc) ==
            waiting_for_disc)
            return 0;
        runtime.resume_endgame_after_disc();
        return 1;
    }

    // Stacked panels are indexed against the palette of the frame below.
    static bool dialog_active_palette(void* host, oa::PaletteBytes* out) {
        auto& runtime = *static_cast<Runtime*>(host);
        if (runtime.screen_ == Screen::match) {
            *out = runtime.match_palette_;
            return true;
        }
        if (!runtime.resources_.background.palette)
            return false;
        *out = *runtime.resources_.background.palette;
        return true;
    }

    // The screen's top panel: the in-match panel (the options panel while
    // paused) on the match canvas, or the frontend screen's root.
    static bool
    dialog_panel_below(void* host, int32_t* x, int32_t* y, int32_t* width, int32_t* height) {
        auto& runtime = *static_cast<Runtime*>(host);
        if (runtime.screen_ == Screen::match) {
            if (!runtime.match_hud_ || runtime.match_hud_->layout.gadgets.empty())
                return false;
            const auto& root = runtime.match_hud_->layout.gadgets.front().common;
            const auto rect = oa::ui::display_layout::source_rect_to_canvas(
                runtime.match_layout_, root.x, root.y, root.width, root.height
            );
            *x = rect.x;
            *y = rect.y;
            *width = rect.width;
            *height = rect.height;
            return true;
        }
        if (runtime.resources_.layout.gadgets.empty())
            return false;
        const auto& root = runtime.resources_.layout.gadgets.front().common;
        *x = root.x;
        *y = root.y;
        *width = root.width;
        *height = root.height;
        return true;
    }

    // Beside-HUD panels centre right of the drawn side column in a match.
    static int32_t dialog_hud_strip_width(void* host) {
        auto& runtime = *static_cast<Runtime*>(host);
        return runtime.screen_ == Screen::match ? runtime.match_layout_.left : 0;
    }

    // The layered match presenter draws the dialogs as a layer of its own.
    static bool dialog_draws_layer(void* host) {
        auto& runtime = *static_cast<Runtime*>(host);
        return runtime.screen_ == Screen::match && runtime.match_use_layers_;
    }

    static void dialog_report(void* host, const char* message) {
        static_cast<Runtime*>(host)->status_ = message;
        std::fprintf(stderr, "%s\n", message);
    }

    // Screen hooks.
    static void enter_main_menu(ScreenContext* ctx, void*) {
        auto& runtime = host(ctx);
        runtime.resources_ = load_main_menu(runtime);
        runtime.state_.state = frontend::state_id::main_menu;
        // The next game's unit limit, should a frontend clear have emptied it.
        Runtime::EngineSettingsState::keep_run_unit_limit(runtime);
        setup_main_menu_panel(runtime);
        // The game reaches the main menu only through the dispatcher,
        // which follows the setup with this step; screens that return here
        // directly run it too. A second run from the dispatcher changes nothing.
        if (const auto* step = step_find(&runtime.screens_, frontend::Step::return_to_main_menu))
            step->run(ctx, step->state);
    }

    // Runs the MAINMENU.GUI setup over the loaded records and
    // writes the version label back to its widget; without `starts_music`
    // the music plays on as it is.
    static void setup_main_menu_panel(Runtime& runtime, bool starts_music = true) {
        // The main menu shows in its picture's palette, which the skirmish
        // setup and the map selection keep.
        runtime.main_menu_palette_ = runtime.resources_.background.palette;
        static ui::frontend::MainMenuChecks checks;
        ui::frontend::MainMenuHost menu{};
        menu.context = &runtime;
        menu.translate = Runtime::translation_hook;
        if (starts_music) {
            menu.play_music = [](void* context, const char* sound) {
                static_cast<Runtime*>(context)->play_menu_voice(sound);
            };
            menu.set_music_kind = [](void* context, int32_t) {
                static_cast<Runtime*>(context)->music_main_menu();
            };
        }
        menu.measure_text = [](void* context, const char* text) {
            return static_cast<int32_t>(oa::formats::fnt::measure_text(
                static_cast<Runtime*>(context)->resources_.font, text
            ));
        };
        menu.reset_sparks = [](void* context) {
            auto& owner = *static_cast<Runtime*>(context);
            renderer::reset_menu_sparks(owner.menu_sparks_, owner.resources_.background);
        };
        menu.foreign_cd_player = [](void* context) {
            return static_cast<Runtime*>(context)->music_foreign_player();
        };
        menu.close_cd_player = [](void* context) {
            static_cast<Runtime*>(context)->music_close_foreign_player();
        };
        menu.sound_driver_missing = [](void* context) {
            return static_cast<Runtime*>(context)->music_no_driver();
        };
        menu.show_message = [](void* context, const char* text, int32_t width) {
            static_cast<Runtime*>(context)->show_frontend_message(
                text, width, entry::message_show_ok, entry::message_fit_width
            );
        };
        menu.movies_present = [](void* context) {
            return static_cast<Runtime*>(context)->offers_movies();
        };
        // A mod shows its own version where the profile names one, whether
        // or not its game data names a revision.
        if (const auto* profile = runtime.options_.mod_profile.get();
            profile != nullptr &&
            profile->identity.display_version != oa::data::mod_profile::Identity{}.display_version)
            menu.version_text = profile->identity.display_version.c_str();
        else
            menu.revision_named = [](void* context) {
                return static_cast<Runtime*>(context)->assets_.file_size(
                           oa::data::defs::data_path(
                               oa::data::defs::DataDirectory::gamedata, "version.tdf"
                           )
                       ) != 0;
            };
        static ui::frontend::Panel panel;
        ui::frontend::panel_load_layout(panel, runtime.resources_.layout);
        ui::frontend::main_menu_setup(panel, checks, menu);
        // The setup's version label, INTRO's gray and Credits' visibility.
        for (const auto name :
             {ui::frontend::kMainMenuVersionControl,
              ui::frontend::kMainMenuIntroControl,
              ui::frontend::kMainMenuCreditsControl}) {
            const auto index = ui::frontend::panel_find(panel, name);
            if (index < 0 ||
                static_cast<std::size_t>(index) >= runtime.resources_.layout.gadgets.size())
                continue;
            const auto& control = panel.controls[static_cast<std::size_t>(index)];
            auto& gadget = runtime.resources_.layout.gadgets[static_cast<std::size_t>(index)];
            gadget.common.active = static_cast<int8_t>(control.active);
            gadget.common.x = control.x;
            if (auto* label = std::get_if<oa::ui::gui_layout::LabelFields>(&gadget.fields))
                label->text = std::string(ui::frontend::control_text(control));
            if (auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields))
                button->grayed_out = control.grayed != 0;
        }
    }

    static void enter_single_player(ScreenContext* ctx, void*) {
        host(ctx).state_.state = frontend::state_id::single_player_menu;
        host(ctx).enter_single_player_panel();
    }

    static void enter_skirmish(ScreenContext* ctx, void*) {
        host(ctx).state_.state = frontend::state_id::skirmish_menu;
    }

    static void enter_loading(ScreenContext* ctx, void*) { host(ctx).ensure_loading_screen(); }

    static void enter_visuals(ScreenContext* ctx, void*) { host(ctx).sync_visual_option_widgets(); }

    // NEWGAME.GUI for a new campaign or any mission.
    static void enter_new_game(ScreenContext* ctx, void*) {
        auto& runtime = host(ctx);
        runtime.enter_new_game_panel(ctx->screen == screen_id(Screen::any_mission));
        runtime.sync_campaign_option_widgets();
    }

    // The save dialog shows on dsavegame2, the load dialog on dloadgame2.
    static const char* load_game_background(ScreenContext* ctx, void*) {
        return host(ctx).load_game_background();
    }

    // Outcome1 while a campaign can continue, else Outcome0.
    static const char* campaign_end_background(ScreenContext* ctx, void*) {
        return host(ctx).campaign_end_background();
    }

    static void enter_campaign_end(ScreenContext* ctx, void*) { host(ctx).enter_campaign_end(); }

    static void leave_campaign_end(ScreenContext* ctx, void*) { host(ctx).leave_campaign_end(); }

    static void enter_load_game(ScreenContext* ctx, void*) { host(ctx).enter_load_game(); }

    static void leave_load_game(ScreenContext* ctx, void*) { host(ctx).leave_load_game(); }

    // Dispatcher steps.
    static void step_setup_main_menu(ScreenContext* ctx, void*) {
        host(ctx).load(Screen::main_menu);
    }

    static void step_setup_single_player(ScreenContext* ctx, void*) {
        host(ctx).load(Screen::single_player);
    }

    static void step_setup_skirmish(ScreenContext* ctx, void*) {
        auto& runtime = host(ctx);
        skirmish::setup(
            runtime.state_,
            runtime.skirmish_settings_,
            runtime.preferences_,
            runtime.skirmish_ui_,
            runtime
        );
    }

    static void step_reset_player_slots(ScreenContext* ctx, void*) {
        auto& runtime = host(ctx);
        init::reset_player_slots(runtime.state_, runtime.player_storage_, false);
    }

    static void step_load_preferences(ScreenContext* ctx, void*) {
        auto& runtime = host(ctx);
        // The player's own skirmish setup comes back from a saved, recorded
        // or network game before the preferences are read over it.
        if (runtime.player_skirmish_settings_) {
            runtime.skirmish_settings_ = std::move(*runtime.player_skirmish_settings_);
            runtime.player_skirmish_settings_.reset();
        }
        init::load_preferences(
            runtime.state_,
            runtime.skirmish_settings_,
            runtime.preferences_,
            runtime,
            view_rules::display_mode_setting(runtime.ui_rules())
        );
    }

    static void step_save_preferences(ScreenContext* ctx, void*) { host(ctx).save_preferences(); }

    static void step_clear_selection(ScreenContext* ctx, void*) { host(ctx).selected_ = -1; }

    static void step_ignore(ScreenContext*, void*) {}

    // The main-menu reset drops the named background.
    static void step_load_default_palette(ScreenContext* ctx, void*) {
        // Selecting none reads no file; whether the backdrop changed is not
        // needed.
        std::ignore = host(ctx).load_named_background(nullptr, false, false, false);
    }

    // The end-game state runs as the ENDMSN.GUI screen's enter.
    static void step_enter_end_mission(ScreenContext* ctx, void*) {
        host(ctx).load(Screen::campaign_end);
    }

    static void register_all(ScreenRegistry* registry);
};

namespace {

constexpr const char* kGuiPalette = "palettes/guipal.pal";
constexpr const char* kCommonGaf = "anims/commongui.gaf";

// A refused registration is recorded in the registry, and register_screens
// reports it once every screen, overlay and step is in.
void add_screen(ScreenRegistry* registry, const ScreenDesc& desc) {
    screen_register(registry, &desc);
}

/// A screen drawn from a GUI layout.
///
/// @param screen the screen
/// @param name its name
/// @param layout its layout's file name in the GUI directory, such as "single.gui"
/// @param background its named background, or null
/// @param sprites its own sprites
/// @return the screen's description
ScreenDesc gui_screen(
    Screen screen, const char* name, const char* layout, const char* background, const char* sprites
) {
    ScreenDesc desc{};
    desc.id = screen_id(screen);
    desc.name = name;
    desc.assets = {layout, background, kGuiPalette, sprites, kCommonGaf};
    return desc;
}

} // namespace

void BuiltinScreens::register_all(ScreenRegistry* registry) {
    ScreenDesc desc{};
    desc.id = screen_id(Screen::main_menu);
    desc.name = "main_menu";
    desc.enter = enter_main_menu;
    add_screen(registry, desc);

    desc = gui_screen(
        Screen::single_player, "single_player", "single.gui", "singlebg", "anims/single.gaf"
    );
    desc.enter = enter_single_player;
    add_screen(registry, desc);

    // SKIRMISH.GUI's setup asks for Skirmsetup4x itself.
    desc = gui_screen(Screen::skirmish, "skirmish", "skirmish.gui", nullptr, "anims/skirmish.gaf");
    desc.enter = enter_skirmish;
    add_screen(registry, desc);

    desc = {};
    desc.id = screen_id(Screen::loading);
    desc.name = "loading";
    desc.enter = enter_loading;
    add_screen(registry, desc);

    add_screen(
        registry, gui_screen(Screen::options, "options", "startopt.gui", "options4x", kCommonGaf)
    );
    add_screen(registry, gui_screen(Screen::sound, "sound", "sound.gui", "optsound4x", kCommonGaf));
    desc = gui_screen(Screen::visuals, "visuals", "visuals.gui", "optvisual4x", kCommonGaf);
    desc.enter = enter_visuals;
    add_screen(registry, desc);
    add_screen(
        registry, gui_screen(Screen::speeds, "speeds", "speeds.gui", "optinterface4x", kCommonGaf)
    );
    add_screen(registry, gui_screen(Screen::music, "music", "sound.gui", "optmusic4x", kCommonGaf));

    // NEWGAME.GUI's setup asks for its background by mode and campaign count.
    desc = gui_screen(
        Screen::new_campaign, "new_campaign", "newgame.gui", nullptr, "anims/newgame.gaf"
    );
    desc.enter = enter_new_game;
    add_screen(registry, desc);
    desc.id = screen_id(Screen::any_mission);
    desc.name = "any_mission";
    add_screen(registry, desc);

    desc = gui_screen(Screen::load_game, "load_game", "loadgame.gui", "dloadgame2", kCommonGaf);
    desc.background = load_game_background;
    desc.enter = enter_load_game;
    desc.leave = leave_load_game;
    add_screen(registry, desc);

    desc =
        gui_screen(Screen::campaign_end, "campaign_end", "endmsn.gui", nullptr, "anims/endmsn.gaf");
    desc.background = campaign_end_background;
    desc.enter = enter_campaign_end;
    desc.leave = leave_campaign_end;
    add_screen(registry, desc);

    // Also the fallback for screens without a registration of their own.
    // The map modal's setup asks for DSELECTMAP2 itself.
    add_screen(
        registry,
        gui_screen(
            Screen::map_selection, "map_selection", "selmap.gui", nullptr, "anims/skirmish.gaf"
        )
    );

    using frontend::Step;
    // A refused step is recorded in the registry, as a refused screen is.
    step_register(registry, Step::setup_main_menu, step_setup_main_menu, nullptr);
    step_register(registry, Step::setup_single_player, step_setup_single_player, nullptr);
    step_register(registry, Step::setup_skirmish, step_setup_skirmish, nullptr);
    step_register(registry, Step::reset_player_slots, step_reset_player_slots, nullptr);
    step_register(registry, Step::load_preferences, step_load_preferences, nullptr);
    step_register(registry, Step::save_preferences, step_save_preferences, nullptr);
    step_register(registry, Step::draw_current_frame, step_clear_selection, nullptr);
    step_register(registry, Step::load_default_palette, step_load_default_palette, nullptr);
    for (const auto ignored :
         {Step::check_state_checksum,
          Step::present_frame,
          Step::get_video_context,
          Step::pop_input_event})
        step_register(registry, ignored, step_ignore, nullptr);
    step_register(registry, Step::enter_end_mission, step_enter_end_mission, nullptr);
}

std::string Runtime::screen_sound_resource(std::string_view name) const {
    if (const auto* sound = audio_registry_.get(audio_registry_.find(name)))
        return sound->resource;
    return oa::audio::game_audio::sound_resource(name);
}

void register_builtin_screens(ScreenRegistry* registry) {
    BuiltinScreens::register_all(registry);
}

namespace {

constexpr ScreenServices kRuntimeServices{
    BuiltinScreens::request_screen,
    BuiltinScreens::play_sound,
    BuiltinScreens::read_number,
    BuiltinScreens::write_number,
    BuiltinScreens::read_string,
    BuiltinScreens::write_string,
    BuiltinScreens::set_status,
    BuiltinScreens::set_frontend_signal,
    BuiltinScreens::set_frontend_state,
    BuiltinScreens::close_main_menu_panel,
    BuiltinScreens::reload_main_menu_panel,
    BuiltinScreens::set_text_input,
    BuiltinScreens::current_tick,
    BuiltinScreens::register_sound,
    BuiltinScreens::pointer_position,
    BuiltinScreens::set_main_menu_overlay,
    BuiltinScreens::quit,
    BuiltinScreens::stop_sounds,
    BuiltinScreens::play_sound_alternate,
    BuiltinScreens::run_frontend
};

bool overlay_applies(const OverlayDesc& overlay, ScreenId screen) {
    return overlay.screen == kScreenAny || overlay.screen == screen;
}

bool translate_input(
    const SDL_Event& event, SDL_Renderer* renderer, uint16_t modifiers, ScreenInput& input
) {
    input = {};
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        input.kind =
            event.type == SDL_EVENT_KEY_DOWN ? ScreenInputKind::key_down : ScreenInputKind::key_up;
        input.key = static_cast<uint32_t>(event.key.key);
        input.modifiers = static_cast<uint16_t>(event.key.mod);
        return true;
    case SDL_EVENT_TEXT_INPUT:
        input.kind = ScreenInputKind::text;
        input.text = event.text.text;
        return true;
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL: {
        SDL_Event converted = event;
        // Headless checks have no renderer and send canvas coordinates.
        if (renderer != nullptr && !convert_event_to_frame(renderer, converted))
            return false;
        input.modifiers = modifiers;
        if (converted.type == SDL_EVENT_MOUSE_MOTION) {
            input.kind = ScreenInputKind::pointer_move;
            input.x = converted.motion.x;
            input.y = converted.motion.y;
        } else if (converted.type == SDL_EVENT_MOUSE_WHEEL) {
            input.kind = ScreenInputKind::wheel;
            input.x = converted.wheel.mouse_x;
            input.y = converted.wheel.mouse_y;
            input.wheel_y = converted.wheel.y;
        } else {
            input.kind = converted.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                             ? ScreenInputKind::pointer_down
                             : ScreenInputKind::pointer_up;
            input.button = converted.button.button;
            input.clicks = converted.button.clicks;
            input.x = converted.button.x;
            input.y = converted.button.y;
        }
        return true;
    }
    default:
        return false;
    }
}

/// Tells whether two overlay descriptions are the same registration.
///
/// @param first one description
/// @param second the other
/// @return true when every field is equal
bool same_overlay(const OverlayDesc& first, const OverlayDesc& second) {
    return first.name == second.name && first.screen == second.screen && first.z == second.z &&
           first.create == second.create && first.event == second.event &&
           first.tick == second.tick && first.draw == second.draw && first.state == second.state;
}

/// Tells whether a list holds an overlay's registration.
///
/// @param overlays the list
/// @param overlay the overlay
/// @return true when one entry is the same registration
bool holds_overlay(const std::vector<OverlayDesc>& overlays, const OverlayDesc& overlay) {
    return std::any_of(overlays.begin(), overlays.end(), [&](const OverlayDesc& other) {
        return same_overlay(overlay, other);
    });
}

} // namespace

void Runtime::register_screens() {
#define OA_REGISTER(fn) fn(&screens_);
#include "oa/ui/screen_registry/screens.inc"
#undef OA_REGISTER
    const std::vector<OverlayDesc> built_in(
        screens_.overlays, screens_.overlays + screens_.overlay_count
    );
    call_hook_or_raise<&Extension::register_screens>(extension_, &screens_);
    extension_overlays_.clear();
    for (uint32_t index = 0; index < screens_.overlay_count; ++index)
        if (!holds_overlay(built_in, screens_.overlays[index]))
            extension_overlays_.push_back(screens_.overlays[index]);
    // The OA button and the settings dialog, on the main menu and in a match.
    register_engine_settings_overlays();
    register_engine_settings_match_overlay();
    // The notice of the saved games' move, over the main menu.
    register_saves_notice_overlay();
    register_mod_install_overlay();
    // Without screens of the extension's for them, the multiplayer unit
    // headers and a main-menu overlay's steps have nothing to do. The registry
    // refuses a second handler, so these fill only the steps nobody took.
    for (const auto unclaimed :
         {frontend::Step::reload_unit_overrides,
          frontend::Step::shut_down_resource,
          frontend::Step::return_to_main_menu})
        if (step_find(&screens_, unclaimed) == nullptr)
            step_register(&screens_, unclaimed, BuiltinScreens::step_ignore, nullptr);
    if (screens_.rejected != nullptr)
        throw std::runtime_error(std::string("screen registration rejected: ") + screens_.rejected);
    if (screen_find(&screens_, screen_id(Screen::map_selection)) == nullptr)
        throw std::runtime_error("built-in screens are not registered");
    for (uint32_t index = 0; index < screens_.overlay_count; ++index) {
        auto& overlay = screens_.overlays[index];
        if (overlay.create != nullptr) {
            auto context = screen_context();
            overlay.create(&context, overlay.state);
        }
    }
    oa::ui::frontend_dialogs::dialogs_bind_host(
        {this,
         BuiltinScreens::dialog_sound,
         translation_hook,
         BuiltinScreens::dialog_cd_check_click,
         BuiltinScreens::dialog_active_palette,
         BuiltinScreens::dialog_panel_below,
         BuiltinScreens::dialog_report,
         BuiltinScreens::dialog_hud_strip_width,
         BuiltinScreens::dialog_draws_layer}
    );
}

void Runtime::reload_main_menu_language() {
    // The same MAINMENU.GUI, so the hovered and pressed buttons and the
    // keyboard's focus keep their records.
    resources_ = BuiltinScreens::load_main_menu(*this);
    BuiltinScreens::setup_main_menu_panel(*this, false);
}

void Runtime::set_extension_overlays_aside(bool aside) {
    if (aside == !overlays_before_aside_.empty())
        return;
    if (!aside) {
        std::copy(overlays_before_aside_.begin(), overlays_before_aside_.end(), screens_.overlays);
        screens_.overlay_count = static_cast<uint32_t>(overlays_before_aside_.size());
        overlays_before_aside_.clear();
        main_menu_overlay_ = main_menu_overlay_before_aside_;
        return;
    }
    overlays_before_aside_.assign(screens_.overlays, screens_.overlays + screens_.overlay_count);
    main_menu_overlay_before_aside_ = main_menu_overlay_;
    uint32_t kept = 0;
    for (const auto& overlay : overlays_before_aside_)
        if (!holds_overlay(extension_overlays_, overlay))
            screens_.overlays[kept++] = overlay;
    screens_.overlay_count = kept;
    main_menu_overlay_ = false;
}

ScreenContext Runtime::screen_context(const ScreenInput* input) {
    return {this, &kRuntimeServices, &assets_, &surface_, match_.get(), input, screen_id(screen_)};
}

bool Runtime::dispatch_screen_input(const SDL_Event& event) {
    const auto current = screen_id(screen_);
    const auto* desc = screen_find(&screens_, current);
    bool wanted = desc != nullptr && desc->event != nullptr;
    for (uint32_t index = 0; !wanted && index < screens_.overlay_count; ++index)
        wanted = screens_.overlays[index].event != nullptr &&
                 overlay_applies(screens_.overlays[index], current);
    if (!wanted)
        return false;
    ScreenInput input{};
    if (!translate_input(
            event,
            sdl_.renderer,
            static_cast<uint16_t>(input_modifiers(ModifierUse::keyboard)),
            input
        ))
        return false;
    auto context = screen_context(&input);
    bool taken = false;
    for (auto index = screens_.overlay_count; !taken && index > 0; --index) {
        auto& overlay = screens_.overlays[index - 1];
        taken = overlay.event != nullptr && overlay_applies(overlay, current) &&
                overlay.event(&context, overlay.state) != 0;
    }
    if (!taken)
        taken =
            desc != nullptr && desc->event != nullptr && desc->event(&context, desc->state) != 0;
    // The built-in handler never sees a taken event, so the cursor sprite
    // follows the pointer here, whether an overlay or the screen took it.
    if (taken && (input.kind == ScreenInputKind::pointer_move ||
                  input.kind == ScreenInputKind::pointer_down ||
                  input.kind == ScreenInputKind::pointer_up)) {
        pointer_x_ = input.x;
        pointer_y_ = input.y;
    }
    return taken;
}

void Runtime::tick_screen_packages() {
    const auto current = screen_id(screen_);
    auto context = screen_context();
    for (uint32_t index = 0; index < screens_.overlay_count; ++index) {
        auto& overlay = screens_.overlays[index];
        if (overlay.tick != nullptr && overlay_applies(overlay, current))
            overlay.tick(&context, overlay.state);
    }
    if (const auto* desc = screen_find(&screens_, current);
        desc != nullptr && desc->tick != nullptr)
        desc->tick(&context, desc->state);
    run_pending_ending();
    run_pending_notice_return();
    apply_screen_request();
}

void Runtime::draw_screen_packages() {
    const auto current = screen_id(screen_);
    auto context = screen_context();
    if (const auto* desc = screen_find(&screens_, current);
        desc != nullptr && desc->draw != nullptr)
        desc->draw(&context, desc->state);
    for (uint32_t index = 0; index < screens_.overlay_count; ++index) {
        auto& overlay = screens_.overlays[index];
        if (overlay.draw != nullptr && overlay_applies(overlay, current))
            overlay.draw(&context, overlay.state);
    }
}

void Runtime::finish_quit_request() {
    if (!quit_requested_)
        return;
    quit_requested_ = false;
    // A running match is left first, as the exit confirmation's first
    // choice leaves it.
    if (match_ && !match_finished_)
        leave_match();
    const std::string reason = std::move(quit_reason_);
    quit_reason_.clear();
    quit_application(reason.c_str());
}

void Runtime::apply_screen_request() {
    // A run a package ended shows nothing more.
    if (quit_requested_) {
        finish_quit_request();
        return;
    }
    const auto load_requested_screen = [this] {
        if (!pending_screen_)
            return;
        const auto id = *pending_screen_;
        pending_screen_.reset();
        load(static_cast<Screen>(id));
    };
    load_requested_screen();
    if (!frontend_pass_requested_)
        return;
    // A request made during the pass asks for the next one.
    frontend_pass_requested_ = false;
    if (screen_ == Screen::match)
        return;
    // The pass a pointer press runs: the unit header step, then the dispatcher.
    step(frontend::Step::reload_unit_overrides, state_);
    frontend::dispatch(state_, *this, frontend_states_);
    load_requested_screen();
}

} // namespace oa::app
