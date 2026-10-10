// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// End-of-game screen over the finished match's Game block. The extension
// hears the match events it reports.
#include "oa/app/runtime.hpp"
#include "device_state.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/ui/decoded.hpp"

#include "oa/ui/frontend_state/app_modes.hpp"
#include "oa/data/campaign/campaign_file.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/surface.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/campaign/endgame.hpp"
#include "oa/ui/campaign/screens.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace oa::app {

namespace campaign = oa::ui::campaign;

namespace {

constexpr uint32_t kEngineTicksPerSecond = 30; // the rate of the clock the end screen runs on
constexpr uint32_t kMillisecondsPerSecond = 1000;
// Dispatcher passes of an ending: its initialize, the movies, then the main
// menu's setup.
constexpr int kEndingDispatchPasses = 3;
// Engine ticks the end screen is stepped for, at most, to reach its panel
// or to leave it.
constexpr int kEndScreenFrameLimit = 4000;
// A darkened last frame keeps at most this share of its brightness: a tenth.
constexpr uint64_t kDarkenedDivisor = 10;
#if OA_SELF_CHECKS
// Match ticks a check runs, at most, for a swept match to reach its outcome.
constexpr uint32_t kOutcomeTickLimit = 30 * 20;
// Pixels around the pointer a presented frame may differ by: the software
// cursor.
constexpr int kCursorReach = 64;
// Where the panel of a game that cannot continue shows Main Menu: in the
// single button housing of the Outcome0 background.
constexpr int16_t finished_main_menu_x = 460;
constexpr int16_t finished_main_menu_y = 416;
#endif

Runtime& runtime_of(void* context) {
    return *static_cast<Runtime*>(context);
}

uint32_t rgb_key(const uint8_t* rgb) {
    return static_cast<uint32_t>(rgb[0]) << 16 | static_cast<uint32_t>(rgb[1]) << 8 | rgb[2];
}

// Maps a composed frame back to palette indices: each pixel takes the lowest
// entry holding its colour, else the nearest by summed channel difference.
void index_frame(
    const renderer::Surface& frame, const oa::PaletteBytes& palette, std::vector<uint8_t>& indices
) {
    std::unordered_map<uint32_t, uint8_t> index_of;
    for (std::size_t entry = oa::palette_color_count; entry-- > 0;)
        index_of[rgb_key(&palette[entry * oa::palette_entry_bytes])] = static_cast<uint8_t>(entry);
    const std::size_t pixels = static_cast<std::size_t>(frame.width) * frame.height;
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const uint8_t* rgb = &frame.rgb[pixel * 3U];
        const auto key = rgb_key(rgb);
        auto found = index_of.find(key);
        if (found == index_of.end()) {
            int32_t best = std::numeric_limits<int32_t>::max();
            uint8_t nearest = 0;
            for (std::size_t entry = 0; entry < oa::palette_color_count; ++entry) {
                const auto* colour = &palette[entry * oa::palette_entry_bytes];
                const int32_t distance = std::abs(colour[0] - rgb[0]) +
                                         std::abs(colour[1] - rgb[1]) +
                                         std::abs(colour[2] - rgb[2]);
                if (distance < best) {
                    best = distance;
                    nearest = static_cast<uint8_t>(entry);
                }
            }
            found = index_of.emplace(key, nearest).first;
        }
        indices[pixel] = found->second;
    }
}

} // namespace

struct Runtime::EndgameState {
    // The finished match: its Game block holds the score table the screen shows.
    std::unique_ptr<oa::sim::match_runtime::Match> match{};
    // Game options record of a skirmish or multiplayer game; a campaign mission
    // uses the campaign's own.
    oa::data::campaign::CampaignFile session{};
    oa::data::campaign::CampaignFile* game_options = nullptr;
    campaign::EndgameScreen screen{};
    campaign::EndgameHost host{};
    campaign::FrontendHost frontend{};
    // The panel was left for a mission briefing, whose Back reopens it.
    bool reopen = false;
    // The glamour picture the outcome step loaded after a campaign victory.
    oa::Image glamour{};
    campaign::EndgamePicture glamour_view{};
    // Frontend state a final campaign victory leaves for after this frame.
    std::optional<uint8_t> ending{};
    // Engine ticks of the navigation check, which steps the screen itself.
    std::optional<uint32_t> stepped_clock{};
    // The finished match's last frame, its outcome title included, in indices
    // of battlefield_palette. The screen shows it and darkens it step by step
    // until the outcome's picture or the panel replaces it.
    oa::present::SurfaceBuffer battlefield{};
    oa::PaletteBytes battlefield_palette{};
    // The last frame keeps the display at its own size until the darkening
    // ends, when the display goes back to the frontend's 640x480.
    bool battlefield_sized = false;
};

void Runtime::destroy_endgame_state(EndgameState* state) noexcept {
    delete state;
}

Runtime::EndgameState& Runtime::endgame_state() {
    if (!endgame_)
        endgame_.reset(new EndgameState());
    return *endgame_;
}

uint32_t Runtime::endgame_now() const {
    if (endgame_ && endgame_->stepped_clock)
        return *endgame_->stepped_clock;
    return static_cast<uint32_t>(SDL_GetTicks() * kEngineTicksPerSecond / kMillisecondsPerSecond);
}

oa::data::campaign::CampaignFile* Runtime::game_options() {
    return campaign_mission_ ? &campaign_object() : &endgame_state().session;
}

void Runtime::bind_session_options() {
    if (campaign_mission_)
        return;
    const auto multiplier = [this](std::string_view key) {
        const auto value = text(key);
        return value ? static_cast<float>(oa::formats::tdf::parse_double(value->c_str())) : 0.0F;
    };
    auto& session = endgame_state().session;
    session.kill_multiplier = multiplier("killmul");
    session.time_multiplier = multiplier("timemul");
}

void Runtime::keep_battlefield_frame() {
    auto& state = endgame_state();
    state.battlefield = {};
    state.battlefield_sized = false;
    if (!match_ || screen_ != Screen::match)
        return;
    // A finish no frame was drawn for, as in a headless run, draws the
    // outcome's frame now.
    if (!outcome_frame_drawn_)
        render_match_surface();
    ensure_screen_world();
    renderer::Surface frame;
    compose_match_layers(frame);
    if (frame.width == 0 || frame.height == 0 ||
        frame.rgb.size() != static_cast<std::size_t>(frame.width) * frame.height * 3U)
        return;
    state.battlefield = oa::present::create_surface(
        static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height)
    );
    index_frame(frame, match_palette_, state.battlefield.pixels);
    state.battlefield_palette = match_palette_;
    state.battlefield_sized = true;
}

void Runtime::shade_battlefield(int32_t level) {
    if (!endgame_ || endgame_->battlefield.pixels.empty())
        return;
    auto& frame = endgame_->battlefield.surface;
    Rect32 whole{0, 0, frame.width - 1, frame.height - 1};
    auto* previous = oa::present::display_context();
    oa::present::bind_display(&display_.context);
    // A display that cannot be locked leaves the battlefield unshaded for
    // this frame alone; the next frame shades it again.
    std::ignore = oa::present::shade_rect_level(&frame, &whole, level);
    oa::present::bind_display(previous);
}

void Runtime::finish_battlefield_shade() {
    if (!endgame_ || !endgame_->battlefield_sized)
        return;
    endgame_->battlefield_sized = false;
    apply_output_mode();
}

const oa::Surface* Runtime::end_screen_battlefield_size() const {
    if (screen_ != Screen::campaign_end || !endgame_ || !endgame_->match ||
        !endgame_->battlefield_sized || endgame_->battlefield.pixels.empty())
        return nullptr;
    return &endgame_->battlefield.surface;
}

bool Runtime::draw_end_screen_battlefield() {
    if (screen_ != Screen::campaign_end || !endgame_ || !endgame_->match)
        return false;
    const auto& state = *endgame_;
    const auto step = state.match->state().game.endgame_state;
    if (step >= OA_ENDGAME_STAT_BARS)
        return false;
    const auto& frame = state.battlefield.surface;
    // After the darkening the display is the frontend's again: a last frame of
    // that size stays, a larger one gives way to a cleared screen.
    const bool shown =
        step != OA_ENDGAME_GLAMOUR && !state.battlefield.pixels.empty() &&
        (state.battlefield_sized || (frame.width == kCanvasWidth && frame.height == kCanvasHeight));
    if (!shown) {
        surface_.width = kCanvasWidth;
        surface_.height = kCanvasHeight;
        surface_.rgb.assign(static_cast<std::size_t>(kCanvasWidth) * kCanvasHeight * 3U, 0);
        return true;
    }
    surface_.width = static_cast<uint32_t>(frame.width);
    surface_.height = static_cast<uint32_t>(frame.height);
    const std::size_t pixels = state.battlefield.pixels.size();
    surface_.rgb.resize(pixels * 3U);
    for (std::size_t pixel = 0; pixel < pixels; ++pixel)
        std::memcpy(
            &surface_.rgb[pixel * 3U],
            &state.battlefield_palette
                 [static_cast<std::size_t>(state.battlefield.pixels[pixel]) *
                  oa::palette_entry_bytes],
            3
        );
    return true;
}

bool Runtime::end_screen_hides_cursor() const {
    if (screen_ != Screen::campaign_end || !endgame_ || !endgame_->match)
        return false;
    return endgame_->match->state().game.endgame_state < OA_ENDGAME_PANEL;
}

void Runtime::keep_finished_match() {
    if (!match_)
        return;
    campaign::publish_endgame({});
    keep_battlefield_frame();
    auto& state = endgame_state();
    state.game_options = game_options();
    if (!campaign_mission_)
        state.session.kind = (current_extension_state() & extension_state::shared_match) != 0
                                 ? oa::data::campaign::SessionKind::multiplayer
                                 : oa::data::campaign::SessionKind::skirmish;
    oa::World& world = match_->state();
    // The match runtime keeps the outcome bits of Game.outcome_flags beside the
    // block; the score table reads the victory bit from the block.
    world.game.outcome_flags |= match_->outcome_state().flags;
    // The session's mission results (Game.mission_results) carry over the match.
    std::copy_n(
        state_.mission_results.begin(),
        sizeof world.game.mission_results - 1,
        world.game.mission_results
    );
    campaign::build_score_summary(world, state.game_options);
    std::copy_n(
        world.game.mission_results,
        sizeof world.game.mission_results - 1,
        state_.mission_results.begin()
    );
    campaign::set_endgame_state(world.game, OA_ENDGAME_CAPTURE);
    world.game.mode = frontend::mode_id::end_game;
    music_end_game();
    state.screen = {};
    state.reopen = false;
    state.glamour = {};
    state.glamour_view = {};
    state.ending.reset();
    call_hook_or_report<&Extension::match_event>(
        extension_, hook_error_report(), *this, MatchEvent::finished
    );
    state.match = std::move(match_);
}

void Runtime::start_endgame() {
    if (!endgame_ || !endgame_->match)
        return;
    auto& state = *endgame_;
    state.frontend = {};
    state.frontend.context = this;
    state.frontend.translate = translation_hook;
    state.frontend.disc_present = [](void* context) {
        return runtime_of(context).find_disc(menu::Disc::campaign) != 0;
    };
    state.frontend.set_control_value = [](void* context, const char* name, int32_t value) {
        if (auto* gadget = runtime_of(context).widget(name))
            gadget->common.active = static_cast<uint8_t>(value);
    };
    state.frontend.set_control_y = set_widget_y;
    state.host = {};
    state.host.context = this;
    state.host.now = [](void* context) { return runtime_of(context).endgame_now(); };
    state.host.ticks_per_second = [](void*) { return kEngineTicksPerSecond; };
    if (extension_.match_event != nullptr)
        state.host.report_end = [](void* context) {
            auto& runtime = runtime_of(context);
            call_hook_or_report<&Extension::match_event>(
                runtime.extension_,
                runtime.hook_error_report(),
                runtime,
                MatchEvent::results_reported
            );
        };
    if (extension_.disconnect_text != nullptr)
        state.host.disconnect_message = [](void* context, uint8_t reason) {
            auto& runtime = runtime_of(context);
            const char* text =
                call_hook_or_raise<&Extension::disconnect_text>(runtime.extension_, reason);
            if (text == nullptr)
                return;
            runtime.show_frontend_message(
                runtime.translate_ui(text),
                campaign::kDisconnectMessageWidth,
                entry::message_show_ok,
                entry::message_fit_width
            );
        };
    state.host.message_open = [](void*) {
        return oa::ui::frontend_dialogs::dialog_kind() ==
               oa::ui::frontend_dialogs::DialogKind::message_box;
    };
    // The last frame of the match was kept as it finished; the screen darkens
    // it step by step and then gives the display back to the frontend.
    state.host.capture_frame = [](void* context) {
        const auto& runtime = runtime_of(context);
        return runtime.endgame_ && !runtime.endgame_->battlefield.pixels.empty();
    };
    state.host.apply_shade = [](void* context, int32_t level) {
        runtime_of(context).shade_battlefield(level);
    };
    state.host.finish_shade = [](void* context) { runtime_of(context).finish_battlefield_shade(); };
    state.host.open_cd_check = [](void* context) { runtime_of(context).show_cd_check(); };
    state.host.open_panel = [](void* context) { runtime_of(context).open_end_panel(nullptr); };
    // A finger resting on the screen holds the button as the left button does.
    state.host.button_held = [](void* context) {
        return (device_state::buttons_held() & SDL_BUTTON_LMASK) != 0 ||
               runtime_of(context).touch_finger_count() != 0;
    };
    state.host.play_sound = [](void* context, const char* name) {
        runtime_of(context).play_ui_sound(name, 0);
    };
    state.host.show_outcome = [](void* context) { runtime_of(context).load_outcome_glamour(); };
    state.host.glamour_palette = [](void* context) {
        return runtime_of(context).outcome_glamour_palette();
    };
    state.host.movies_enabled = [](void* context) {
        return (runtime_of(context).state_.video_context_flags & frontend::flags::intro_enabled) !=
               0;
    };
    state.host.play_ending = [](void* context, uint8_t next) {
        auto& runtime = runtime_of(context);
        if (!runtime.endgame_)
            return;
        // Game data that offers no movies, such as the Total Annihilation demo
        // (1997), ends the campaign with a notice over the main menu instead.
        if (!runtime.offers_movies()) {
            next = frontend::state_id::main_menu;
            runtime.ending_notice_pending_ = true;
        }
        runtime.endgame_->ending = next;
    };
    state.host.play_stream = [](void* context, const char* path, int32_t, uint32_t delay) {
        runtime_of(context).play_briefing_narration(path, delay);
    };
    state.host.stop_stream = [](void* context) { runtime_of(context).stop_briefing_audio(); };
    state.host.frontend = &state.frontend;
    campaign::EndgameView view{};
    view.world = &state.match->state();
    view.campaign = state.game_options;
    view.screen = &state.screen;
    view.host = &state.host;
    view.font = &resources_.font;
    view.label_font = resources_.label_font.glyphs['I'] ? &resources_.label_font : &resources_.font;
    view.palette = resources_.background.palette ? resources_.background.palette->data()
                                                 : resources_.gui_palette.data();
    view.light_table = resources_.light_table.empty() ? nullptr : resources_.light_table.data();
    view.player_logos = gaf_sequence(resources_.global_sprites, "32xlogos");
    view.glamour = &state.glamour_view;
    campaign::publish_endgame(view);
    if (state.reopen) {
        state.reopen = false;
        open_end_panel(&state.screen.layout);
    }
}

oa::World* Runtime::endgame_world() {
    return endgame_ && endgame_->match ? &endgame_->match->state() : nullptr;
}

oa::data::campaign::CampaignFile* Runtime::endgame_game_options() {
    return endgame_ && endgame_->match ? endgame_->game_options : nullptr;
}

void Runtime::leave_end_panel_for_briefing() {
    if (endgame_)
        endgame_->reopen = true;
}

void Runtime::load_outcome_glamour() {
    if (!endgame_ || !endgame_->match)
        return;
    auto& state = *endgame_;
    state.glamour = {};
    state.glamour_view = {};
    const oa::Game& game = state.match->state().game;
    const auto env = campaign_object_env();
    campaign::OutcomeBackground background{};
    campaign::choose_outcome_background(
        state.game_options,
        (game.outcome_flags & sim::scenario::outcome_flag::won) != 0,
        env.files,
        &background
    );
    if (background.glamour[0] == '\0')
        return;
    try {
        const auto path = std::string("bitmaps/") + background.glamour;
        state.glamour = oa::ui::decoded::require(oa::decode_pcx(assets_.read(path).bytes), path);
    } catch (const std::exception& error) {
        std::cerr << "glamour picture unavailable: " << error.what() << '\n';
        state.glamour = {};
        return;
    }
    const auto pixels = static_cast<std::size_t>(state.glamour.width) * state.glamour.height;
    if (!state.glamour.palette || state.glamour.indices.size() != pixels) {
        state.glamour = {};
        return;
    }
    state.glamour_view = {state.glamour.indices.data(), state.glamour.width, state.glamour.height};
}

const uint8_t* Runtime::outcome_glamour_palette() const {
    if (!endgame_ || endgame_->glamour_view.pixels == nullptr)
        return nullptr;
    return endgame_->glamour.palette->data();
}

void Runtime::run_pending_ending() {
    if (!endgame_ || !endgame_->ending)
        return;
    const uint8_t next = *endgame_->ending;
    endgame_->ending.reset();
    frontend::set_frontend_state(state_, *this, next);
    set_app_mode(state_, frontend::mode_id::frontend);
    for (int pass = 0; pass < kEndingDispatchPasses && screen_ == Screen::campaign_end; ++pass) {
        step(frontend::Step::reload_unit_overrides, state_);
        frontend::dispatch(state_, *this, frontend_states_);
    }
    if (ending_notice_pending_) {
        ending_notice_pending_ = false;
        show_missing_content(MissingContent::further_missions);
    }
}

bool Runtime::holds_movies() const {
    std::error_code error;
    for (const auto& root : assets_.loose_roots())
        for (const auto& folder : fs::directory_iterator(root, error)) {
            std::string name = path_to_utf8(folder.path().filename());
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (name != "data" || !folder.is_directory(error))
                continue;
            for (const auto& file : fs::directory_iterator(folder.path(), error)) {
                std::string extension = path_to_utf8(file.path().extension());
                std::transform(
                    extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    }
                );
                if (extension == ".zrb")
                    return true;
            }
        }
    return false;
}

bool Runtime::offers_movies() const {
    return holds_disc_archive() || holds_movies();
}

bool Runtime::endgame_reopens() const {
    return endgame_ && endgame_->match && endgame_->reopen;
}

void Runtime::release_endgame() {
    campaign::publish_endgame({});
    if (!endgame_)
        return;
    call_hook_or_report<&Extension::match_event>(
        extension_, hook_error_report(), *this, MatchEvent::results_released
    );
    endgame_->match.reset();
    endgame_->reopen = false;
    endgame_->glamour = {};
    endgame_->glamour_view = {};
    endgame_->ending.reset();
    endgame_->battlefield = {};
    endgame_->battlefield_sized = false;
}

void Runtime::resume_endgame_after_disc() {
    if (endgame_ && endgame_->match) {
        campaign::set_endgame_state(endgame_->match->state().game, OA_ENDGAME_OUTCOME);
        return;
    }
    finish_match_outcome();
}

void Runtime::set_endgame_state(frontend::State&, int32_t step) {
    if (endgame_ && endgame_->match)
        campaign::set_endgame_state(endgame_->match->state().game, static_cast<uint8_t>(step));
}

void Runtime::check_endgame_screen(const std::filesystem::path& report_directory) {
    if (!endgame_ || !endgame_->match)
        throw std::runtime_error("the finished mission kept no Game block for the end screen");
    {
        const oa::Game& game = endgame_->match->state().game;
        ensure_ui_colors();
        if (!std::equal(ui_colors_.begin(), ui_colors_.end(), game.ui_colors) ||
            game.ui_colors[4] == game.ui_colors[8])
            throw std::runtime_error("the kept Game block lacks the session's UI colour table");
    }
    check_end_screen_darkening(report_directory);
    const auto steps = step_end_screen_to_panel(
        [&] {
            rebuild_surface();
            write_ppm(report_directory / "native-campaign-cdcheck.ppm", surface_);
        },
        [&] { check_glamour_frame(report_directory); }
    );
    if (steps.glamour_refused)
        throw std::runtime_error("the end screen did not accept glamour input");
    if (!steps.glamour_pressed)
        throw std::runtime_error("the won campaign mission did not show its glamour picture");
    if (!endgame_ || !endgame_->match)
        throw std::runtime_error("the won campaign mission left its end screen");
    auto& state = *endgame_;
    const oa::Game& game = state.match->state().game;
    if (!steps.panel)
        throw std::runtime_error(
            "the end screen did not reach its panel (state=" + std::to_string(game.endgame_state) +
            ", rows=" + std::to_string(state.screen.layout.row_count) + ")"
        );
    const auto* start = widget("Start");
    if (start == nullptr || start->common.active == 0)
        throw std::runtime_error("the won campaign mission did not offer Start");
    rebuild_surface();
    const auto& row = state.screen.layout.rows[0];
    const auto& palette =
        resources_.background.palette ? *resources_.background.palette : resources_.gui_palette;
    const auto* fill = palette.data() + static_cast<std::size_t>(game.ui_colors[4]) * 4U;
    const auto* shown =
        surface_.rgb.data() + (static_cast<std::size_t>(row.y + 2) * surface_.width +
                               static_cast<std::size_t>(row.bars[0].x + 2)) *
                                  3U;
    if (!std::equal(fill, fill + 3, shown))
        throw std::runtime_error("the stat bars are not filled in UI colour 4");
    std::cout << "end screen check: " << state.screen.layout.row_count << " score rows"
              << (steps.disc_check ? " after the disc check" : "") << '\n';
}

void Runtime::check_end_screen_darkening(const std::filesystem::path& report_directory) {
    auto& state = *endgame_;
    const oa::Game& game = state.match->state().game;
    const auto& kept = state.battlefield;
    if (game.endgame_state != OA_ENDGAME_CAPTURE || kept.pixels.empty() || !state.battlefield_sized)
        throw std::runtime_error("the end screen did not keep the match's last frame");
    const auto sized_as_kept = [&] {
        return surface_.width == static_cast<uint32_t>(kept.surface.width) &&
               surface_.height == static_cast<uint32_t>(kept.surface.height);
    };
    const auto brightness = [this] {
        uint64_t sum = 0;
        for (const uint8_t channel : surface_.rgb)
            sum += channel;
        return sum;
    };
    rebuild_surface();
    if (!sized_as_kept())
        throw std::runtime_error("the end screen did not open at the match's size");
    for (std::size_t pixel = 0; pixel < kept.pixels.size(); ++pixel) {
        const auto* expected =
            &state.battlefield_palette
                 [static_cast<std::size_t>(kept.pixels[pixel]) * oa::palette_entry_bytes];
        if (!std::equal(expected, expected + 3, &surface_.rgb[pixel * 3U]))
            throw std::runtime_error("the end screen did not open on the match's last frame");
    }
    write_ppm(report_directory / "native-campaign-outcome.ppm", surface_);
    const uint64_t opened = brightness();
    uint64_t previous = opened;
    int32_t steps = 0;
    std::optional<int32_t> countdown{};
    bool halfway_written = false;
    state.stepped_clock = endgame_now();
    for (int frame = 0; frame < kEndScreenFrameLimit && game.endgame_state <= OA_ENDGAME_SHADING;
         ++frame) {
        ++*state.stepped_clock;
        tick_screen_packages();
        if (screen_ != Screen::campaign_end || !endgame_ || !endgame_->match)
            throw std::runtime_error("the end screen left before the match's last frame was dark");
        rebuild_surface();
        if (game.endgame_state <= OA_ENDGAME_SHADING && !sized_as_kept())
            throw std::runtime_error(
                "the end screen left the match's size before the frame was dark"
            );
        const uint64_t shown = brightness();
        if (shown > previous)
            throw std::runtime_error("the match's last frame grew brighter on the end screen");
        previous = shown;
        if (game.endgame_state == OA_ENDGAME_SHADING ||
            game.endgame_state == OA_ENDGAME_DISC_CHECK) {
            if (countdown && game.endgame_shade_countdown < *countdown)
                ++steps;
            const int32_t shade_countdown = game.endgame_shade_countdown;
            countdown = shade_countdown;
        }
        if (steps == campaign::kShadeSteps / 2 && !halfway_written) {
            write_ppm(report_directory / "native-campaign-darkening.ppm", surface_);
            halfway_written = true;
        }
    }
    if (steps != campaign::kShadeSteps || game.endgame_state <= OA_ENDGAME_SHADING)
        throw std::runtime_error(
            "the end screen darkened the match's last frame in " + std::to_string(steps) +
            " steps, not " + std::to_string(campaign::kShadeSteps)
        );
    if (previous * kDarkenedDivisor > opened)
        throw std::runtime_error(
            "the match's last frame is not dark once the end screen darkened it"
        );
    if (state.battlefield_sized)
        throw std::runtime_error("the display kept the match's size after the darkening");
    std::cout << "end screen darkening check: the match's last frame, " << kept.surface.width << 'x'
              << kept.surface.height << ", darkened in " << steps << " steps to "
              << previous * 100U / std::max<uint64_t>(opened, 1U) << "% of its brightness\n";
}

// A step of the self-checks (runtime_checks.cpp), which a build without them
// leaves out.
#if OA_SELF_CHECKS
void Runtime::check_presented_match_end(const std::filesystem::path& report_directory) {
    if (screen_ != Screen::match || !match_)
        throw std::runtime_error("match end check: no match to finish");
    const oa::World& world = match_->state();
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player)
        if (player != match_local_player_ && world.game.players[player].unit_count != 0)
            match_->destroy_player_units(player);
    for (uint32_t tick = 0;
         tick < kOutcomeTickLimit && match_->outcome() == sim::scenario::Outcome::ongoing;
         ++tick)
        step_match_simulation();
    if (match_->outcome() != sim::scenario::Outcome::victory)
        throw std::runtime_error("match end check: the opponents' sweep did not win the skirmish");
    renderer::Surface presented;
    const auto present_frame = [&] {
        capture_frame_ = &presented;
        idle_tick();
        capture_frame_ = nullptr;
    };
    const auto brightness = [&] {
        uint64_t sum = 0;
        for (const uint8_t channel : presented.rgb)
            sum += channel;
        return sum;
    };
    present_frame();
    write_ppm(report_directory / "native-match-end-outcome.ppm", presented);
    if (screen_ != Screen::campaign_end || !endgame_ || !endgame_->match)
        throw std::runtime_error("match end check: the won skirmish stayed on its outcome frame");
    const auto& kept = endgame_->battlefield.surface;
    if (endgame_->battlefield.pixels.empty() ||
        presented.width != static_cast<uint32_t>(kept.width) ||
        presented.height != static_cast<uint32_t>(kept.height))
        throw std::runtime_error(
            "match end check: the end screen kept no frame of the match's size"
        );
    // The kept frame is the one the outcome was presented on, the cursor aside.
    rebuild_surface();
    auto expected = surface_;
    apply_gamma_rgb(expected.rgb.data(), expected.rgb.size() / 3U, 3);
    std::size_t differing = 0;
    for (int y = 0; y < kept.height; ++y)
        for (int x = 0; x < kept.width; ++x) {
            if (std::abs(x - static_cast<int>(pointer_x_)) < kCursorReach &&
                std::abs(y - static_cast<int>(pointer_y_)) < kCursorReach)
                continue;
            const auto at = (static_cast<std::size_t>(y) * presented.width + x) * 3U;
            if (!std::equal(&expected.rgb[at], &expected.rgb[at] + 3, &presented.rgb[at]))
                ++differing;
        }
    if (differing != 0)
        throw std::runtime_error(
            "match end check: " + std::to_string(differing) +
            " pixels of the kept frame differ from the presented outcome frame"
        );
    const oa::Game& game = endgame_->match->state().game;
    std::optional<uint64_t> previous{};
    int frames = 0;
    for (; frames < kEndScreenFrameLimit && game.endgame_state <= OA_ENDGAME_SHADING; ++frames) {
        present_frame();
        if (screen_ != Screen::campaign_end || !endgame_ || !endgame_->match)
            throw std::runtime_error("match end check: the end screen left while it darkened");
        if (game.endgame_state > OA_ENDGAME_SHADING)
            break;
        if (presented.width != static_cast<uint32_t>(kept.width) ||
            presented.height != static_cast<uint32_t>(kept.height))
            throw std::runtime_error(
                "match end check: the darkening was not shown at the match's size"
            );
        const uint64_t shown = brightness();
        if (previous && shown > *previous)
            throw std::runtime_error("match end check: the darkened frame grew brighter");
        previous = shown;
    }
    if (game.endgame_state <= OA_ENDGAME_SHADING)
        throw std::runtime_error("match end check: the end screen did not finish darkening");
    std::cout << "match end check: the won skirmish left its " << kept.width << 'x' << kept.height
              << " VICTORY frame for the end screen, which darkened it over " << frames
              << " presented frames\n";
    if (!step_end_screen_to_panel().panel)
        throw std::runtime_error("match end check: the end screen reached no panel");
    rebuild_surface();
    write_ppm(report_directory / "native-match-end-panel.ppm", surface_);
    // The statistics keep the game's own fonts whatever the Language settings say.
    require_game_fonts("match end check: the statistics screen", [this] { rebuild_surface(); });
    const auto* main_menu = widget("MainMenu");
    const auto* start = widget("Start");
    if (main_menu == nullptr || main_menu->common.active == 0 ||
        main_menu->common.x != finished_main_menu_x ||
        main_menu->common.y != finished_main_menu_y || start == nullptr ||
        start->common.active != 0)
        throw std::runtime_error(
            "match end check: the panel does not show Main Menu alone in Outcome0's button housing"
        );
    std::cout << "match end check: the panel shows Main Menu alone at " << main_menu->common.x
              << ',' << main_menu->common.y << '\n';
}
#endif

bool Runtime::step_endgame_until_left() {
    if (!endgame_ || !endgame_->match)
        return false;
    endgame_->stepped_clock = endgame_now();
    for (int frame = 0; frame < kEndScreenFrameLimit && screen_ == Screen::campaign_end; ++frame) {
        ++*endgame_->stepped_clock;
        tick_screen_packages();
    }
    endgame_->stepped_clock.reset();
    return screen_ != Screen::campaign_end;
}

Runtime::EndScreenSteps Runtime::step_end_screen_to_panel(
    const std::function<void()>& at_disc_check, const std::function<void()>& at_glamour
) {
    EndScreenSteps steps;
    if (!endgame_ || !endgame_->match)
        return steps;
    endgame_->stepped_clock = endgame_now();
    for (int frame = 0; frame < kEndScreenFrameLimit; ++frame) {
        if (!endgame_ || !endgame_->match || !endgame_->stepped_clock)
            return steps;
        auto& state = *endgame_;
        // A final campaign victory leaves for the ending instead of the panel.
        if (state.ending) {
            state.stepped_clock.reset();
            run_pending_ending();
            return steps;
        }
        const oa::Game& game = state.match->state().game;
        if (oa::ui::frontend_dialogs::dialog_kind() ==
            oa::ui::frontend_dialogs::DialogKind::cd_check) {
            if (at_disc_check)
                at_disc_check();
            oa::ui::frontend_dialogs::close_dialog();
            resume_endgame_after_disc();
            steps.disc_check = true;
            continue;
        }
        // The glamour picture waits for an input event after its fade.
        // Deliver one through the screen overlay, as a player would.
        if (!steps.glamour_pressed && game.endgame_state == OA_ENDGAME_GLAMOUR &&
            game.endgame_fade_done != 0 && game.endgame_next_tick < *state.stepped_clock) {
            if (at_glamour)
                at_glamour();
            SDL_Event press{};
            press.type = SDL_EVENT_KEY_DOWN;
            press.key.key = SDLK_SPACE;
            steps.glamour_pressed = true;
            if (!dispatch_screen_input(press)) {
                steps.glamour_refused = true;
                break;
            }
        }
        if (game.endgame_state == OA_ENDGAME_PANEL && state.screen.layout.row_count != 0) {
            state.stepped_clock.reset();
            steps.panel = true;
            return steps;
        }
        ++*state.stepped_clock;
        tick_screen_packages();
    }
    if (endgame_)
        endgame_->stepped_clock.reset();
    return steps;
}

void Runtime::check_glamour_frame(const std::filesystem::path& report_directory) {
    const auto& picture = endgame_->glamour_view;
    if (picture.pixels == nullptr || picture.width < surface_.width ||
        picture.height < surface_.height)
        throw std::runtime_error("the won campaign mission loaded no full-screen glamour picture");
    rebuild_surface();
    write_ppm(report_directory / "native-campaign-glamour.ppm", surface_);
    const std::size_t x = surface_.width / 2;
    const std::size_t y = surface_.height / 2;
    const auto* expected = endgame_->glamour.palette->data() +
                           static_cast<std::size_t>(picture.pixels[y * picture.width + x]) * 4U;
    const auto* shown = surface_.rgb.data() + (y * surface_.width + x) * 3U;
    if (!std::equal(expected, expected + 3, shown))
        throw std::runtime_error("the glamour picture is not shown in its palette");
    std::cout << "glamour check: " << picture.width << 'x' << picture.height << " picture shown\n";
}

} // namespace oa::app
