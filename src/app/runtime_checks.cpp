// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Bounded headless navigation checks.
#include "oa/app/runtime.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/data/defs/layout.hpp"
#include "map_picture_state.hpp"
#include "oa/app/asset_files.hpp"
#include "oa/ui/hud/order_panel.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/data/persist/save_sections.hpp"
#include "oa/sim/messages.hpp"
#include "oa/sim/speed.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"
#include "oa/sim/state_hash.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/present/world_renderer/world_overlays.hpp"
#include "match_fault.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

// The window the composed-frame check lays the match out for.
constexpr int kWindowCheckWidth = 2000;
constexpr int kWindowCheckHeight = 1109;
// A radar picture shows terrain: many colours, not a black well with the
// white camera outline.
constexpr std::size_t kRadarMinColours = 16;
constexpr std::size_t kRadarMinLitPercent = 50;
// Distance from the pointer that covers every cursor frame.
constexpr int kCursorReach = 64;
// Distance from the pointer that holds a metal extractor's build outline at
// the checked window sizes.
constexpr int kOutlineReach = 64;
// Pixels the build outline must change: about one of its two rings around a
// two-cell (32 pixel) footprint.
constexpr std::size_t kOutlineMinPixels = 4 * 32;
// Ticks the victory check allows the sweep and the outcome countdown.
constexpr uint32_t kVictoryTickLimit = 30 * 20;

// Map pixels either side of the local unit the selection check puts its two
// kbots at, and canvas pixels the drag box's corners may sit from the press
// and the pointer (the terrain under them is rounded to whole pixels).
constexpr int kSelectionSpread = 40;
constexpr int kDragCornerSlack = 2;
// The drag box starts this far up and left of the group and ends this far
// inside the lower right corner of the battlefield (or of the map, where it
// ends first), so that it crosses the fog.
constexpr int kDragMargin = 48;
constexpr int kDragEdgeInset = 48;
// Share of a selection box's outline that must show UI colour 10 once the
// unit is selected; the unit, drawn after its box, hides the rest.
constexpr std::size_t kSelectionBoxMinPercent = 25;

// Map pixels below the local unit the pixel particle check starts a nano
// stream at, and how far right of that the stream aims.
constexpr int kParticleDrop = 48;
constexpr int kParticleReach = 64;

// The zooms the scene draw scale check draws at, the game's own first, and
// the draw scale its scene is drawn at apart from the world layer.
constexpr std::array<float, 3> kSceneCheckZooms{1.0F, 0.5F, 2.0F};
constexpr float kSceneCheckDrawScale = 1.0F;
// 64-bit FNV-1a's starting value and multiplier, over the piece transforms.
constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

// Where the overlay check's probe draws, in 640x480 pixels: a line of text
// 63 above the bottom bar and 1 right of the battlefield's edge, and under it
// a 65 by 9 bar, where the game's traffic readout starts; and a line in the
// message log's font further right.
constexpr int kProbeInset = 1;
constexpr int kProbeRise = 63;
constexpr int kProbeBarWidth = 65;
constexpr int kProbeBarHeight = 9;
constexpr int kProbeMessageInset = 130;
// Width of the probe's text lines, in 640x480 pixels.
constexpr int kProbeTextWidth = 120;
constexpr const char* kProbeText = "match overlay check";

// What the overlay check's probe saw on its calls.
struct OverlayProbe {
    int calls{};
    uint8_t side_panel_height{};
    uint8_t message_log_height{};
    uint8_t bar_color{};
};

/// Returns the overlay check's probe record.
///
/// @return the record, which lives until the process exits
OverlayProbe& overlay_probe() {
    static OverlayProbe probe;
    return probe;
}

/// Draws the overlay check's marks (Extension::draw_match_overlay).
///
/// @param overlay the battlefield and its painter
void draw_overlay_probe(void* /*context*/, Runtime& /*runtime*/, const MatchOverlay& overlay) {
    auto& probe = overlay_probe();
    ++probe.calls;
    probe.side_panel_height = overlay.font_height(overlay.painter, OverlayFont::side_panel);
    probe.message_log_height = overlay.font_height(overlay.painter, OverlayFont::message_log);
    const int scale = overlay.scale;
    const int x = overlay.left + kProbeInset * scale;
    const int y = overlay.bottom - kProbeRise * scale;
    overlay.draw_text(overlay.painter, OverlayFont::side_panel, x, y, kProbeText, probe.bar_color);
    overlay.fill_rect(
        overlay.painter,
        x,
        y + probe.side_panel_height * scale,
        kProbeBarWidth * scale,
        kProbeBarHeight * scale,
        probe.bar_color
    );
    overlay.draw_text(
        overlay.painter,
        OverlayFont::message_log,
        overlay.left + kProbeMessageInset * scale,
        y,
        kProbeText,
        probe.bar_color
    );
}

// What the service checks' probe hooks answer and what they saw. The probes
// stand in for the extension's hooks, whose context stays the extension's,
// so they keep their state here.
struct ServiceProbe {
    bool close_answer{};     // close_requested's answer
    int close_requests{};    // close requests offered
    int entry_calls{};       // frontend_entry calls
    const char* nickname{};  // FrontendEntry::nickname given
    const char* game_name{}; // FrontendEntry::game_name given
    const char* label{};     // return_label's answer
    int left_events{};       // MatchEvent::left heard
    int setup_runs{};        // runs of the main menu's setup step
    uint32_t query_value{};  // what the probe query answers
    StepDesc setup_step{};   // the setup step's own binding, run after the count
    void (*match_event)(void* context, Runtime& runtime, MatchEvent event){}; // the extension's
};

/// Returns the service checks' probe record.
///
/// @return the record, which lives until the process exits
ServiceProbe& service_probe() {
    static ServiceProbe probe;
    return probe;
}

/// Answers a close request as the probe says, counting it (Extension::close_requested).
///
/// @return the probe's answer
bool probe_close_requested(void* /*context*/, Runtime& /*runtime*/) {
    auto& probe = service_probe();
    ++probe.close_requests;
    return probe.close_answer;
}

/// Gives the probe's nickname and game name, counting the call (Extension::frontend_entry).
///
/// @param[out] entry the launch values
void probe_frontend_entry(void* /*context*/, FrontendEntry& entry) {
    auto& probe = service_probe();
    ++probe.entry_calls;
    entry.nickname = probe.nickname;
    entry.game_name = probe.game_name;
}

/// Answers the return label as the probe says (Extension::return_label).
///
/// @return the probe's label
const char* probe_return_label(void* /*context*/) {
    return service_probe().label;
}

/// Counts MatchEvent::left, then passes every event on to the extension's hook.
///
/// @param context Extension::context, passed on
/// @param[in,out] runtime the running app
/// @param event what happened
void probe_match_event(void* context, Runtime& runtime, MatchEvent event) {
    auto& probe = service_probe();
    if (event == MatchEvent::left)
        ++probe.left_events;
    if (probe.match_event != nullptr)
        probe.match_event(context, runtime, event);
}

/// Counts a run of the main menu's setup step, then runs the step's own binding.
///
/// @param ctx the step's context
void probe_setup_step(ScreenContext* ctx, void* /*state*/) {
    auto& probe = service_probe();
    ++probe.setup_runs;
    probe.setup_step.run(ctx, probe.setup_step.state);
}

/// Answers the probe query with the probe's value.
///
/// @param ctx the query's context
/// @return the probe's value; 0 without the runtime's services
uint32_t probe_query(ScreenContext* ctx, void* /*state*/) {
    return ctx != nullptr && ctx->host != nullptr && ctx->services != nullptr
               ? service_probe().query_value
               : 0;
}

/// Returns the binding of the main menu's setup step in a registry.
///
/// @param[in,out] registry the runtime's registry
/// @return the binding, or null when the step has none
StepDesc* setup_step_binding(ScreenRegistry& registry) {
    for (uint32_t index = 0; index < registry.step_count; ++index)
        if (registry.steps[index].step == frontend::Step::setup_main_menu)
            return &registry.steps[index];
    return nullptr;
}

bool contains(CanvasRect rect, int x, int y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

bool pixel_is(const renderer::Surface& frame, int x, int y, const std::array<uint8_t, 3>& rgb) {
    if (x < 0 || y < 0 || x >= static_cast<int>(frame.width) || y >= static_cast<int>(frame.height))
        return false;
    const auto* pixel =
        frame.rgb.data() +
        (static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x)) * 3U;
    return pixel[0] == rgb[0] && pixel[1] == rgb[1] && pixel[2] == rgb[2];
}

// Canvas pixels of the line a to b, one per step of the longer axis.
std::vector<std::array<int, 2>> line_pixels(int x0, int y0, int x1, int y1) {
    const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    std::vector<std::array<int, 2>> pixels;
    for (int step = 0; step <= steps; ++step) {
        const double t = steps == 0 ? 0.0 : static_cast<double>(step) / steps;
        pixels.push_back(
            {x0 + static_cast<int>(std::lround((x1 - x0) * t)),
             y0 + static_cast<int>(std::lround((y1 - y0) * t))}
        );
    }
    return pixels;
}

} // namespace

std::vector<uint8_t> copy_rect(const renderer::Surface& frame, CanvasRect rect) {
    std::vector<uint8_t> pixels;
    for (int y = std::max(0, rect.y); y < std::min(static_cast<int>(frame.height), rect.y + rect.h);
         ++y)
        for (int x = std::max(0, rect.x);
             x < std::min(static_cast<int>(frame.width), rect.x + rect.w);
             ++x) {
            const auto* pixel =
                frame.rgb.data() +
                (static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x)) * 3U;
            pixels.insert(pixels.end(), pixel, pixel + 3);
        }
    return pixels;
}

std::size_t changed_pixels(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after) {
    std::size_t changed = 0;
    for (std::size_t i = 0; i + 2 < std::min(before.size(), after.size()); i += 3)
        changed +=
            before[i] != after[i] || before[i + 1] != after[i + 1] || before[i + 2] != after[i + 2]
                ? 1
                : 0;
    return changed;
}

void Runtime::check_match_overlays(
    const std::function<void(renderer::Surface&)>& frame_of, const fs::path& snapshot
) {
    // The log starts empty so every line the check posts is on screen, and
    // gets its earlier lines back afterwards.
    auto& game = match_->state().game;
    char saved_lines[OA_CHAT_LINE_COUNT][OA_CHAT_LINE_BYTES];
    std::memcpy(saved_lines, game.chat_lines, sizeof saved_lines);
    const auto saved_head = game.chat_head;
    const auto saved_tail = game.chat_tail;
    oa::sim::messages::clear_messages(game);
    renderer::Surface frame;
    bool running = true;
    const auto key = [&](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = code;
        event.key.scancode = scancode;
        handle_sdl_event(event, running);
    };
    const auto type = [&](const char* text) {
        SDL_Event event{};
        event.type = SDL_EVENT_TEXT_INPUT;
        event.text.text = text;
        handle_sdl_event(event, running);
    };
    const auto enter_line = [&](const char* text) {
        key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        type(text);
        key(SDLK_RETURN, SDL_SCANCODE_RETURN);
    };
    const auto require_change = [&](CanvasRect rect,
                                    const std::vector<uint8_t>& before,
                                    const char* what,
                                    std::size_t minimum = kTextMinPixels) {
        const auto changed = changed_pixels(before, copy_rect(frame, rect));
        if (changed < minimum)
            throw std::runtime_error(
                std::string("match overlay check: ") + what + " changed " +
                std::to_string(changed) + " pixels of the frame"
            );
        return changed;
    };
    // With mapping and line of sight off the radar shows the whole map
    // picture, so a black well cannot pass for an unexplored one.
    auto& visibility = match_->state().game.visibility_flags;
    const auto saved_visibility = visibility;
    visibility = static_cast<uint8_t>(
        visibility & ~(oa::ui::console::visibility_flag::mapping |
                       oa::ui::console::visibility_flag::line_of_sight)
    );
    // The radar redraws for the changed rules as after a sight rebuild; the
    // match's own grids are left as they were.
    reset_sight_presentation(false);
    frame_of(frame);
    const CanvasRect radar{
        radar_picture_.x, radar_picture_.y, radar_picture_.width, radar_picture_.height
    };
    const auto radar_pixels = copy_rect(frame, radar);
    std::set<uint32_t> colours;
    std::size_t lit = 0;
    for (std::size_t i = 0; i + 2 < radar_pixels.size(); i += 3) {
        const auto rgb = (static_cast<uint32_t>(radar_pixels[i]) << 16) |
                         (static_cast<uint32_t>(radar_pixels[i + 1]) << 8) | radar_pixels[i + 2];
        colours.insert(rgb);
        lit += rgb != 0 ? 1 : 0;
    }
    const auto radar_area = radar_pixels.size() / 3U;
    if (radar_area == 0 || colours.size() < kRadarMinColours ||
        lit * 100U < radar_area * kRadarMinLitPercent)
        throw std::runtime_error(
            "match overlay check: the radar picture is not visible (" +
            std::to_string(colours.size()) + " colours, " + std::to_string(lit) + " of " +
            std::to_string(radar_area) + " pixels lit)"
        );
    const int half_width = match_layout_.battlefield_width() / 2;
    const int band = match_layout_.top;
    const auto log = message_log_rect(
        static_cast<std::size_t>(std::max(0, oa::sim::messages::line_capacity(game)))
    );
    const CanvasRect clock{
        match_layout_.left, match_layout_.bottom_bar_y() - band, half_width, band
    };
    const CanvasRect bar{
        match_layout_.left,
        match_layout_.bottom_bar_y(),
        match_layout_.width - match_layout_.left,
        match_layout_.bottom
    };
    auto before = copy_rect(frame, log);
    console_post_message("match overlay check message");
    frame_of(frame);
    const auto log_changed = require_change(log, before, "a posted message");
    before = copy_rect(frame, clock);
    enter_line("+clock");
    frame_of(frame);
    const auto clock_changed = require_change(clock, before, "the +clock game clock");
    const CanvasRect title{
        (match_layout_.width + match_layout_.left) / 2 - band,
        match_layout_.height / 2 - band / 2,
        2 * band,
        band
    };
    before = copy_rect(frame, title);
    match_paused_ = true;
    frame_of(frame);
    match_paused_ = false;
    const auto title_changed = require_change(title, before, "the paused title");
    frame_of(frame);
    // '+' posts the speed line; at the fastest speed '-' does instead. The
    // other key puts the starting speed back afterwards.
    const uint16_t saved_speed = game.requested_speed;
    const bool raise = saved_speed < oa::sim::speed::fastest;
    const char* speed_key = raise ? "'+'" : "'-'";
    before = copy_rect(frame, log);
    if (raise)
        key(SDLK_EQUALS, SDL_SCANCODE_EQUALS);
    else
        key(SDLK_MINUS, SDL_SCANCODE_MINUS);
    char speed_line[oa::sim::speed::message_bytes];
    oa::sim::speed::format_message(speed_line, game.requested_speed, message_hooks());
    if (const auto lines = match_message_lines();
        game.requested_speed == saved_speed || lines.empty() || lines.back() != speed_line)
        throw std::runtime_error(
            std::string("match overlay check: ") + speed_key + " posted no game speed line"
        );
    frame_of(frame);
    const auto speed_changed = require_change(log, before, "the game speed line");
    // Build mode with the pointer over the middle of the battlefield outlines
    // the viewer's side's metal extractor there.
    const auto saved_pointer_x = pointer_x_;
    const auto saved_pointer_y = pointer_y_;
    const auto saved_match_pointer_x = match_pointer_x_;
    const auto saved_match_pointer_y = match_pointer_y_;
    const auto saved_command = match_command_;
    const auto saved_build_type = pending_build_type_;
    const auto extractor = oa::sim::unit_spawn::find_type_index(
        spawn_type_names_, match_side_prefix() == "cor" ? "CORMEX" : "ARMMEX"
    );
    if (extractor == 0 || extractor >= spawn_types_.size())
        throw std::runtime_error("match overlay check lacks the side's metal extractor");
    const int site_x = match_layout_.left + match_layout_.battlefield_width() / 2;
    const int site_y = match_layout_.top + match_layout_.battlefield_height() / 2;
    update_pointer(static_cast<float>(site_x), static_cast<float>(site_y));
    frame_of(frame);
    const CanvasRect outline{
        site_x - kOutlineReach, site_y - kOutlineReach, 2 * kOutlineReach, 2 * kOutlineReach
    };
    const auto without_outline = copy_rect(frame, outline);
    match_command_ = MatchCommand::build;
    pending_build_type_ = extractor;
    frame_of(frame);
    const auto outline_changed =
        require_change(outline, without_outline, "the build outline", kOutlineMinPixels);
    before = copy_rect(frame, bar);
    key(SDLK_RETURN, SDL_SCANCODE_RETURN);
    if (!chat_composing_)
        throw std::runtime_error("match overlay check: Enter did not open the chat line");
    type("match overlay check");
    frame_of(frame);
    write_ppm(snapshot, frame);
    const auto bar_changed = require_change(bar, before, "the open chat line");
    // Only that the outline changed is checked; by how much is not needed.
    std::ignore = require_change(
        outline, without_outline, "the build outline under the chat line", kOutlineMinPixels
    );
    close_chat_line();
    match_command_ = saved_command;
    pending_build_type_ = saved_build_type;
    update_pointer(saved_match_pointer_x, saved_match_pointer_y);
    pointer_x_ = saved_pointer_x;
    pointer_y_ = saved_pointer_y;
    enter_line("+clock");
    if (raise)
        key(SDLK_MINUS, SDL_SCANCODE_MINUS);
    else
        key(SDLK_EQUALS, SDL_SCANCODE_EQUALS);
    if (game.requested_speed != saved_speed)
        throw std::runtime_error(
            "match overlay check: the speed keys did not restore the starting speed"
        );
    std::memcpy(game.chat_lines, saved_lines, sizeof saved_lines);
    game.chat_head = saved_head;
    game.chat_tail = saved_tail;
    visibility = saved_visibility;
    reset_sight_presentation(false);
    // An extension's overlay shows over the battlefield: the probe's bar
    // holds its colour in every pixel, and its two lines of text show.
    const int scale = hud_text_scale();
    const auto* side_font = overlay_font(OverlayFont::side_panel);
    const uint8_t side_height =
        side_font != nullptr ? static_cast<uint8_t>(side_font->nominal_height) : 0;
    const uint8_t message_height = static_cast<uint8_t>(message_font().nominal_height);
    const int probe_x = match_layout_.left + kProbeInset * scale;
    const int probe_y = match_layout_.bottom_bar_y() - kProbeRise * scale;
    const CanvasRect probe_text{
        probe_x, probe_y, kProbeTextWidth * scale, std::max<int>(side_height, 1) * scale
    };
    const CanvasRect probe_bar{
        probe_x, probe_y + side_height * scale, kProbeBarWidth * scale, kProbeBarHeight * scale
    };
    const CanvasRect probe_message{
        match_layout_.left + kProbeMessageInset * scale,
        probe_y,
        kProbeTextWidth * scale,
        std::max<int>(message_height, 1) * scale
    };
    frame_of(frame);
    const auto text_before = copy_rect(frame, probe_text);
    const auto bar_before = copy_rect(frame, probe_bar);
    const auto message_before = copy_rect(frame, probe_message);
    const auto saved_overlay = extension_.draw_match_overlay;
    auto& probe = overlay_probe();
    probe = {};
    probe.bar_color = game.ui_colors[oa::present::world_renderer::ui_color_traffic_bar];
    extension_.draw_match_overlay = draw_overlay_probe;
    frame_of(frame);
    extension_.draw_match_overlay = saved_overlay;
    write_ppm(
        fs::path(snapshot).replace_filename(snapshot.stem().string() + "-overlay.ppm"), frame
    );
    if (probe.calls != 1 || probe.side_panel_height != side_height ||
        probe.message_log_height != message_height || side_height == 0)
        throw std::runtime_error(
            "match overlay check: the overlay hook ran " + std::to_string(probe.calls) +
            " times with font heights " + std::to_string(probe.side_panel_height) + " and " +
            std::to_string(probe.message_log_height) + ", expected once with " +
            std::to_string(side_height) + " and " + std::to_string(message_height)
        );
    const auto bar_offset = static_cast<std::size_t>(probe.bar_color) * 4U;
    const std::array<uint8_t, 3> bar_rgb{
        match_palette_[bar_offset], match_palette_[bar_offset + 1], match_palette_[bar_offset + 2]
    };
    std::size_t bar_pixels = 0;
    for (int y = probe_bar.y; y < probe_bar.y + probe_bar.h; ++y)
        for (int x = probe_bar.x; x < probe_bar.x + probe_bar.w; ++x)
            bar_pixels += pixel_is(frame, x, y, bar_rgb) ? 1 : 0;
    const auto bar_area = static_cast<std::size_t>(probe_bar.w) * probe_bar.h;
    const auto bar_drawn = changed_pixels(bar_before, copy_rect(frame, probe_bar));
    if (bar_pixels != bar_area || bar_drawn == 0)
        throw std::runtime_error(
            "match overlay check: the overlay's bar shows in " + std::to_string(bar_pixels) +
            " of its " + std::to_string(bar_area) + " pixels at " + std::to_string(probe_bar.x) +
            ',' + std::to_string(probe_bar.y)
        );
    const auto overlay_text_changed = require_change(probe_text, text_before, "the overlay's text");
    const auto overlay_message_changed = require_change(
        probe_message, message_before, "the overlay's text in the message log's font"
    );
    std::cout << "match overlay check: radar " << colours.size() << " colours; message log "
              << log_changed << ", clock " << clock_changed << ", paused title " << title_changed
              << ", " << speed_key << " speed line " << speed_changed << ", build outline "
              << outline_changed << ", chat line " << bar_changed << ", extension overlay bar "
              << bar_pixels << " and text " << overlay_text_changed << " and "
              << overlay_message_changed << " pixels at " << match_layout_.width << 'x'
              << match_layout_.height << '\n';
}

void Runtime::check_composed_frame() {
    const auto saved_layout = match_layout_;
    match_layout_ = lay_out_match(kWindowCheckWidth, kWindowCheckHeight);
    check_match_overlays(
        [this](renderer::Surface& frame) {
            render_match_surface();
            compose_match_frame(frame);
        },
        fs::path("local/reports") / "native-match-composed.ppm"
    );
    match_layout_ = saved_layout;
    render_match_surface();
}

void Runtime::check_loading_sink() {
    const auto previous_screen = screen_;
    load_progress_ = {100, 70, 40, 15, 0, 0};
    loading_flash_.fill(0);
    screen_ = Screen::loading;
    apply_output_mode();
    ensure_loading_screen();
    enter_loading_display();
    renderer::Surface presented;
    capture_frame_ = &presented;
    render();
    capture_frame_ = nullptr;
    const auto& frame = display_.context.back_buffer;
    // The read-back covers the letterboxed area only.
    SDL_FRect area{};
    if (!SDL_GetRenderLogicalPresentationRect(sdl_.renderer, &area) || area.w < kCanvasWidth ||
        area.h < kCanvasHeight)
        throw std::runtime_error(
            "loading sink check: the frame is not letterboxed at 640x480 or more"
        );
    const auto& palette = display_.context.device_palette;
    const CanvasRect cursor{
        static_cast<int>(pointer_x_) - kCursorReach,
        static_cast<int>(pointer_y_) - kCursorReach,
        2 * kCursorReach,
        2 * kCursorReach
    };
    std::size_t differing = 0;
    for (int y = 0; y < frame.height; ++y)
        for (int x = 0; x < frame.width; ++x) {
            if (contains(cursor, x, y))
                continue;
            const auto window_x = static_cast<std::size_t>(
                (static_cast<float>(x) + 0.5F) * area.w / static_cast<float>(frame.width)
            );
            const auto window_y = static_cast<std::size_t>(
                (static_cast<float>(y) + 0.5F) * area.h / static_cast<float>(frame.height)
            );
            if (window_x >= presented.width || window_y >= presented.height)
                throw std::runtime_error(
                    "loading sink check: the " + std::to_string(presented.width) + 'x' +
                    std::to_string(presented.height) + " read-back misses the letterbox at " +
                    std::to_string(area.x) + ',' + std::to_string(area.y) + ' ' +
                    std::to_string(area.w) + 'x' + std::to_string(area.h)
                );
            const auto& entry =
                palette.entries[frame.pixels[static_cast<std::size_t>(y) * frame.pitch + x]];
            const auto* pixel = presented.rgb.data() + (window_y * presented.width + window_x) * 3U;
            differing += pixel[0] != entry.r || pixel[1] != entry.g || pixel[2] != entry.b ? 1 : 0;
        }
    if (differing != 0)
        throw std::runtime_error(
            "loading sink check: " + std::to_string(differing) +
            " pixels differ from the 8-bit frame"
        );
    screen_ = previous_screen;
    apply_output_mode();
    std::cout << "loading sink check: " << frame.width << 'x' << frame.height << " frame shown at "
              << area.w << 'x' << area.h << ", read back " << presented.width << 'x'
              << presented.height << '\n';
}

std::size_t Runtime::presented_pixels_differing(
    const renderer::Surface& presented,
    const renderer::Surface& composed,
    int left_out_x,
    int left_out_y,
    int left_out_size,
    std::string_view label
) const {
    if (presented.width != composed.width || presented.height != composed.height)
        throw std::runtime_error(
            std::string(label) + ": presented " + std::to_string(presented.width) + 'x' +
            std::to_string(presented.height) + ", composed " + std::to_string(composed.width) +
            'x' + std::to_string(composed.height)
        );
    const CanvasRect left_out{left_out_x, left_out_y, left_out_size, left_out_size};
    std::size_t differing = 0;
    for (int y = 0; y < static_cast<int>(composed.height); ++y)
        for (int x = 0; x < static_cast<int>(composed.width); ++x) {
            if (contains(left_out, x, y))
                continue;
            const auto offset =
                (static_cast<std::size_t>(y) * composed.width + static_cast<std::size_t>(x)) * 3U;
            differing += std::equal(
                             presented.rgb.begin() + static_cast<std::ptrdiff_t>(offset),
                             presented.rgb.begin() + static_cast<std::ptrdiff_t>(offset + 3),
                             composed.rgb.begin() + static_cast<std::ptrdiff_t>(offset)
                         )
                             ? 0
                             : 1;
        }
    return differing;
}

void Runtime::expect_presented_equals_composed(const fs::path& directory, std::string_view label) {
    renderer::Surface presented;
    capture_frame_ = &presented;
    render();
    capture_frame_ = nullptr;
    if (presented.rgb.empty())
        throw std::runtime_error(std::string(label) + ": no frame was presented");
    renderer::Surface composed;
    compose_match_frame(composed);
    const std::size_t differing = presented_pixels_differing(
        presented,
        composed,
        static_cast<int>(match_pointer_x_) - kCursorReach,
        static_cast<int>(match_pointer_y_) - kCursorReach,
        2 * kCursorReach,
        label
    );
    if (differing == 0)
        return;
    write_ppm(directory / (std::string(label) + "-composed.ppm"), composed);
    write_ppm(directory / (std::string(label) + "-presented.ppm"), presented);
    throw std::runtime_error(
        std::string(label) + ": " + std::to_string(differing) +
        " presented pixels differ from compose_match_frame"
    );
}

void Runtime::check_match_layers() {
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    check_loading_sink();
    start_benchmark_skirmish();
    // The cursor waits in the blank corner right of the bottom bar until the
    // overlay check moves it over the battlefield.
    update_pointer(
        static_cast<float>(match_layout_.width - 1), static_cast<float>(match_layout_.height - 1)
    );
    std::size_t frames = 0;
    const auto presented_frame = [&](renderer::Surface& presented) {
        const int cursor_x = static_cast<int>(match_pointer_x_) - kCursorReach;
        const int cursor_y = static_cast<int>(match_pointer_y_) - kCursorReach;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        renderer::Surface composed;
        compose_match_frame(composed);
        const std::size_t differing = presented_pixels_differing(
            presented, composed, cursor_x, cursor_y, 2 * kCursorReach, "match layer check"
        );
        if (differing != 0) {
            write_ppm(report_directory / "native-match-layers-composed.ppm", composed);
            write_ppm(report_directory / "native-match-layers-presented.ppm", presented);
            throw std::runtime_error(
                "match layer check: " + std::to_string(differing) +
                " presented pixels differ from compose_match_frame"
            );
        }
        ++frames;
    };
    check_match_overlays(presented_frame, report_directory / "native-match-presented.ppm");
    check_selection_visuals(presented_frame);
    check_pixel_particles(report_directory);
    check_scene_draw_scale(report_directory);
    std::cout << "match layer check: " << frames << " presented frames equal compose_match_frame\n";
    check_presented_match_end(report_directory);
}

void Runtime::check_pixel_particles(const fs::path& report_directory) {
    namespace fx = oa::sim::effect_particles;
    uint16_t anchor = 0;
    for (const auto& slot : match_->world().slots)
        if (slot.unit_index != 0 && slot.unit != nullptr && slot.record.type_index != 0 &&
            oa::world_unit_at(&match_->state(), slot.unit_index)->owner_index ==
                match_local_player_) {
            anchor = slot.unit_index;
            break;
        }
    if (anchor == 0)
        throw std::runtime_error("pixel particle check found no local unit");
    const auto* unit = match_->world().slots[anchor].unit;
    const auto nozzle_x = static_cast<uint32_t>((unit->position[0] >> 16));
    const auto nozzle_z = static_cast<uint32_t>((unit->position[2] >> 16) + kParticleDrop);
    const FixedVec3 nozzle{
        std::bit_cast<int32_t>(nozzle_x << 16),
        std::bit_cast<int32_t>(
            static_cast<uint32_t>(match_->map_height(nozzle_x << 16, nozzle_z << 16)) << 16
        ),
        std::bit_cast<int32_t>(nozzle_z << 16)
    };
    const FixedVec3 aim{
        std::bit_cast<int32_t>((nozzle_x + kParticleReach) << 16), nozzle.y, nozzle.z
    };
    // A random stream of zeros puts the whole first spray at the nozzle,
    // leaving the match's own streams alone.
    const fx::EffectHost zeros{nullptr, [](void*) { return 0; }};
    const auto saved_zoom = match_zoom_;
    const auto saved_zoom_target = match_zoom_target_;
    const auto saved_camera_x = match_camera_x_;
    const auto saved_camera_z = match_camera_z_;
    const auto saved_effects = std::make_unique<fx::EffectWorld>(match_->effects());
    std::string report;
    // Each zoom and the side of the square a particle fills there: two
    // pixels at the game's own scale, twice that at zoom 2.
    for (const auto& [zoom, side] : {std::pair{1.0F, 2}, std::pair{2.0F, 4}}) {
        match_zoom_ = match_zoom_target_ = zoom;
        center_camera_on_unit(anchor);
        render_match_surface();
        const auto without = match_world_cpu_;
        fx::spawn_nano_outward(
            match_->effects(), match_->state().game, zeros, nozzle, {aim, aim}, fx::layer_nano
        );
        // The layer draws its newest emitter last: the last pixel is the
        // spray's top one.
        std::optional<fx::ParticleDraw> drawn;
        fx::draw_layer(
            match_->effects(),
            fx::layer_nano,
            &drawn,
            [](void* context, const fx::ParticleDraw& item) {
                if (item.kind == fx::DrawKind::pixel)
                    *static_cast<std::optional<fx::ParticleDraw>*>(context) = item;
            }
        );
        render_match_surface();
        const auto with = match_world_cpu_;
        renderer::Surface frame;
        compose_match_frame(frame);
        write_ppm(
            report_directory /
                ("native-pixel-particle-zoom-" + std::to_string(static_cast<int>(zoom)) + ".ppm"),
            frame
        );
        match_->effects() = *saved_effects;
        const auto name = "at zoom " + std::to_string(static_cast<int>(zoom));
        if (!drawn || drawn->position.x != nozzle.x || drawn->position.z != nozzle.z)
            throw std::runtime_error("pixel particle check: no spray at the nozzle " + name);
        auto view = live_viewport(match_camera_x_, match_camera_z_);
        view.destination_x = 0;
        view.destination_y = 0;
        view.surface_width = with.width;
        view.surface_height = with.height;
        const auto colour = palette_rgb(drawn->color);
        // The square drawn on a frame against the frame drawn without it: it
        // must be filled with the particle's colour and nothing else changed.
        const auto check_square = [&](const renderer::Surface& frame,
                                      const renderer::Surface& before,
                                      oa::present::world_renderer::ScreenPoint at,
                                      int square,
                                      const std::string& where) {
            if (at.x < 0 || at.y < 0 || at.x + square > static_cast<int>(frame.width) ||
                at.y + square > static_cast<int>(frame.height))
                throw std::runtime_error(
                    "pixel particle check: the nozzle is off the " + where + ' ' + name
                );
            std::size_t filled = 0;
            std::size_t strays = 0;
            for (int y = 0; y < static_cast<int>(frame.height); ++y)
                for (int x = 0; x < static_cast<int>(frame.width); ++x) {
                    const auto offset =
                        (static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x)) *
                        3U;
                    const std::array<uint8_t, 3> pixel{
                        frame.rgb[offset], frame.rgb[offset + 1], frame.rgb[offset + 2]
                    };
                    if (x >= at.x && x < at.x + square && y >= at.y && y < at.y + square)
                        filled += pixel == colour ? 1 : 0;
                    else
                        strays += std::equal(
                                      pixel.begin(),
                                      pixel.end(),
                                      before.rgb.begin() + static_cast<std::ptrdiff_t>(offset)
                                  )
                                      ? 0
                                      : 1;
                }
            if (filled != static_cast<std::size_t>(square * square) || strays != 0)
                throw std::runtime_error(
                    "pixel particle check: " + std::to_string(filled) + " of the " +
                    std::to_string(square) + 'x' + std::to_string(square) + " square at " +
                    std::to_string(at.x) + ',' + std::to_string(at.y) + " filled and " +
                    std::to_string(strays) + " pixels changed outside it on the " + where + ' ' +
                    name
                );
        };
        const std::array<uint32_t, 3> position{
            std::bit_cast<uint32_t>(drawn->position.x),
            std::bit_cast<uint32_t>(drawn->position.y),
            std::bit_cast<uint32_t>(drawn->position.z)
        };
        check_square(with, without, project_match_point(view, position), side, "battlefield");

        // The scene drawn at 1 apart from the world layer: the particle fills
        // its square at the scene's own scale on the scene, and the resample
        // into the world layer a square as many times larger, at as many
        // times its place on the scene, as the zoom is to the draw scale.
        const auto ratio = static_cast<int>(zoom / kSceneCheckDrawScale);
        const auto scaling = oa::app::world_scaling(
            zoom,
            static_cast<int32_t>(with.width),
            static_cast<int32_t>(with.height),
            kSceneCheckDrawScale
        );
        render_match_surface_at(kSceneCheckDrawScale);
        const auto apart_without = match_world_cpu_;
        const auto scene_without = match_scene_cpu_;
        fx::spawn_nano_outward(
            match_->effects(), match_->state().game, zeros, nozzle, {aim, aim}, fx::layer_nano
        );
        render_match_surface_at(kSceneCheckDrawScale);
        const auto apart_with = match_world_cpu_;
        const auto scene_with = match_scene_cpu_;
        match_->effects() = *saved_effects;
        write_ppm(
            report_directory / ("native-pixel-particle-zoom-" +
                                std::to_string(static_cast<int>(zoom)) + "-apart.ppm"),
            apart_with
        );
        if (scene_with.width != static_cast<uint32_t>(scaling.scene_width) ||
            scene_with.height != static_cast<uint32_t>(scaling.scene_height) ||
            apart_with.width != with.width || apart_with.height != with.height)
            throw std::runtime_error(
                "pixel particle check: the scene was not drawn apart at " +
                std::to_string(scaling.scene_width) + 'x' + std::to_string(scaling.scene_height) +
                ' ' + name
            );
        auto scene_view = view;
        scene_view.scale = kSceneCheckDrawScale;
        scene_view.width = scene_with.width;
        scene_view.height = scene_with.height;
        scene_view.surface_width = scene_with.width;
        scene_view.surface_height = scene_with.height;
        const auto on_scene = project_match_point(scene_view, position);
        check_square(scene_with, scene_without, on_scene, side / ratio, "scene");
        check_square(
            apart_with,
            apart_without,
            {on_scene.x * ratio, on_scene.y * ratio},
            side,
            "world layer drawn apart"
        );
        report += std::string(report.empty() ? "" : ", ") + std::to_string(side) + 'x' +
                  std::to_string(side) + ' ' + name;
    }
    match_zoom_ = saved_zoom;
    match_zoom_target_ = saved_zoom_target;
    match_camera_x_ = saved_camera_x;
    match_camera_z_ = saved_camera_z;
    match_scene_cpu_ = {};
    render_match_surface_at(std::nullopt);
    std::cout << "pixel particle check: a nano particle fills " << report
              << ", on the battlefield and on its scene drawn at 1 apart and resampled\n";
}

void Runtime::render_match_surface_at(std::optional<float> draw_scale) {
    scene_draw_scale_ = draw_scale;
    try {
        refresh_filtered_terrain();
        render_match_surface();
    } catch (...) {
        scene_draw_scale_.reset();
        throw;
    }
    scene_draw_scale_.reset();
}

Runtime::DrawReadBack Runtime::match_draw_read_back() {
    DrawReadBack read;
    // The Game block but its resource readout, which eases toward the
    // stores once a draw, whatever the draw.
    read.game = std::make_unique<oa::Game>(match_->state().game);
    read.game->resource_readout = {};
    read.on_screen = on_screen_units_;
    // Every unit's piece transforms, 64-bit FNV-1a over their words.
    read.pieces = kFnvOffsetBasis;
    const auto mix = [&read](int32_t word) {
        const auto bits = std::bit_cast<uint32_t>(word);
        for (int shift = 0; shift < 32; shift += 8) {
            read.pieces ^= (bits >> shift) & 0xffU;
            read.pieces *= kFnvPrime;
        }
    };
    for (const auto& slot : match_->world().slots) {
        if (slot.unit_index == 0)
            continue;
        auto* instance = match_->instance(slot.unit_index);
        if (instance == nullptr)
            continue;
        for (const auto& piece : instance->model().pieces()) {
            mix(piece.transformed_origin.x);
            mix(piece.transformed_origin.y);
            mix(piece.transformed_origin.z);
            for (const auto& vertex : piece.transformed_vertices) {
                mix(vertex.x);
                mix(vertex.y);
                mix(vertex.z);
            }
        }
    }
    read.world = oa::sim::trace::match_state_hash(
        *match_, match_timing_, match_camera_x_, match_camera_z_, nullptr
    );
    return read;
}

const char*
Runtime::draw_read_back_difference(const DrawReadBack& first, const DrawReadBack& second) {
    if (std::memcmp(first.game.get(), second.game.get(), sizeof(oa::Game)) != 0)
        return "the Game block";
    if (first.on_screen != second.on_screen)
        return "the on-screen list";
    if (first.pieces != second.pieces)
        return "the piece transforms";
    if (first.world != second.world)
        return "the world's digest";
    return nullptr;
}

void Runtime::check_scene_draw_scale(const fs::path& report_directory) {
    uint16_t anchor = 0;
    for (const auto& slot : match_->world().slots)
        if (slot.unit_index != 0 && slot.unit != nullptr && slot.record.type_index != 0 &&
            oa::world_unit_at(&match_->state(), slot.unit_index)->owner_index ==
                match_local_player_) {
            anchor = slot.unit_index;
            break;
        }
    if (anchor == 0)
        throw std::runtime_error("scene draw scale check found no local unit");

    const auto read_back = [this] { return match_draw_read_back(); };
    const auto saved_zoom = match_zoom_;
    const auto saved_zoom_target = match_zoom_target_;
    const auto saved_camera_x = match_camera_x_;
    const auto saved_camera_z = match_camera_z_;
    std::string report;
    for (const float zoom : kSceneCheckZooms) {
        match_zoom_ = match_zoom_target_ = zoom;
        center_camera_on_unit(anchor);
        const auto name = "at zoom " + std::to_string(zoom).substr(0, 3);
        // A first draw settles the view at the zoom; the second is the frame
        // as the game draws it.
        render_match_surface_at(std::nullopt);
        render_match_surface_at(std::nullopt);
        const auto drawn = match_world_cpu_;
        const auto drawn_read = read_back();
        const auto scaling = oa::app::world_scaling(
            zoom,
            static_cast<int32_t>(drawn.width),
            static_cast<int32_t>(drawn.height),
            kSceneCheckDrawScale
        );
        render_match_surface_at(kSceneCheckDrawScale);
        const auto apart = match_world_cpu_;
        const auto apart_read = read_back();
        if (!scaling.apart ||
            match_scene_cpu_.width != static_cast<uint32_t>(scaling.scene_width) ||
            match_scene_cpu_.height != static_cast<uint32_t>(scaling.scene_height) ||
            apart.width != drawn.width || apart.height != drawn.height)
            throw std::runtime_error(
                "scene draw scale check: the scene was not drawn apart at " +
                std::to_string(scaling.scene_width) + 'x' + std::to_string(scaling.scene_height) +
                " into a " + std::to_string(drawn.width) + 'x' + std::to_string(drawn.height) +
                " world layer " + name
            );
        if (const char* differs = draw_read_back_difference(apart_read, drawn_read);
            differs != nullptr)
            throw std::runtime_error(
                std::string("scene draw scale check: the draw left ") + differs +
                " otherwise than the frame drawn at the zoom " + name
            );
        if (zoom == 1.0F && apart.rgb != drawn.rgb) {
            std::size_t differing = 0;
            for (std::size_t at = 0; at + 2 < drawn.rgb.size(); at += 3)
                differing += std::equal(
                                 drawn.rgb.begin() + static_cast<std::ptrdiff_t>(at),
                                 drawn.rgb.begin() + static_cast<std::ptrdiff_t>(at + 3),
                                 apart.rgb.begin() + static_cast<std::ptrdiff_t>(at)
                             )
                                 ? 0
                                 : 1;
            write_ppm(report_directory / "native-scene-draw-scale-drawn.ppm", drawn);
            write_ppm(report_directory / "native-scene-draw-scale-apart.ppm", apart);
            throw std::runtime_error(
                "scene draw scale check: " + std::to_string(differing) +
                " pixels of the scene drawn at 1 and resampled differ from the world layer " + name
            );
        }
        report += std::string(report.empty() ? "" : ", ") + std::to_string(scaling.scene_width) +
                  'x' + std::to_string(scaling.scene_height) + ' ' + name;
    }
    match_zoom_ = saved_zoom;
    match_zoom_target_ = saved_zoom_target;
    match_camera_x_ = saved_camera_x;
    match_camera_z_ = saved_camera_z;
    match_scene_cpu_ = {};
    render_match_surface_at(std::nullopt);
    std::cout << "scene draw scale check: scenes drawn at 1 apart, " << report
              << ", leave what the match reads as the zoom's frame does; at zoom 1 the world layer "
                 "is the same\n";
}

void Runtime::check_selection_visuals(const std::function<void(renderer::Surface&)>& frame_of) {
    namespace wr = oa::present::world_renderer;
    auto& slots = match_->world().slots;
    oa::World& world = match_->state();
    const auto record = [&](uint16_t id) -> const oa::Unit& {
        return *oa::world_unit_at(&world, id);
    };
    uint16_t anchor = 0;
    for (const auto& slot : slots)
        if (slot.unit_index != 0 && slot.unit != nullptr && slot.record.type_index != 0 &&
            record(slot.unit_index).owner_index == match_local_player_ &&
            match_->instance(slot.unit_index) != nullptr) {
            anchor = slot.unit_index;
            break;
        }
    if (anchor == 0)
        throw std::runtime_error("selection check found no local unit");
    const auto kbot = oa::sim::unit_spawn::find_type_index(
        spawn_type_names_, match_side_prefix() == "cor" ? "CORAK" : "ARMPW"
    );
    if (kbot == 0)
        throw std::runtime_error("selection check lacks the side's kbot");
    std::vector<uint16_t> group{anchor};
    for (const int dx : {-kSelectionSpread, kSelectionSpread}) {
        const auto x = static_cast<int32_t>(slots[anchor].unit->position[0] >> 16) + dx;
        const auto z = static_cast<int32_t>(slots[anchor].unit->position[2] >> 16);
        oa::sim::unit_spawn::Request request;
        request.player = match_local_player_;
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
        auto* slot = match_->create(request);
        if (slot == nullptr || slot->unit == nullptr)
            throw std::runtime_error("selection check could not add a kbot");
        group.push_back(slot->unit_index);
    }
    const auto saved_selection = selected_local_ids();
    const auto saved_primary = selected_match_unit_;
    const auto saved_command = match_command_;
    const auto saved_zoom = match_zoom_;
    const auto saved_zoom_target = match_zoom_target_;
    const auto saved_pointer_x = match_pointer_x_;
    const auto saved_pointer_y = match_pointer_y_;
    const auto saved_camera_x = match_camera_x_;
    const auto saved_camera_z = match_camera_z_;
    match_zoom_ = match_zoom_target_ = kDefaultBattlefieldZoom;
    match_command_ = MatchCommand::none;
    clear_local_selection();
    center_camera_on_unit(anchor);
    ensure_ui_colors();
    renderer::Surface frame;
    frame_of(frame);

    bool running = true;
    const auto mouse = [&](SDL_EventType type, int x, int y) {
        float window_x = static_cast<float>(x) + 0.5F;
        float window_y = static_cast<float>(y) + 0.5F;
        if (sdl_.renderer != nullptr &&
            !frame_to_window(sdl_.renderer, window_x, window_y, &window_x, &window_y))
            throw std::runtime_error(
                std::string("SDL_RenderCoordinatesToWindow: ") + SDL_GetError()
            );
        SDL_Event event{};
        event.type = type;
        const auto window = sdl_.window != nullptr ? SDL_GetWindowID(sdl_.window) : 0;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion.windowID = window;
            event.motion.x = window_x;
            event.motion.y = window_y;
            event.motion.state = match_drag_ ? SDL_BUTTON_LMASK : 0;
        } else {
            event.button.windowID = window;
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
        }
        handle_sdl_event(event, running);
    };
    const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
    int left = match_layout_.left + match_layout_.battlefield_width();
    int top = match_layout_.top + match_layout_.battlefield_height();
    for (const auto id : group) {
        const auto at = project_match_point(viewport, slots[id].unit->position);
        left = std::min(left, at.x);
        top = std::min(top, at.y);
    }
    const int press_x = std::max(match_layout_.left + kDragEdgeInset, left - kDragMargin);
    const int press_y = std::max(match_layout_.top + kDragEdgeInset, top - kDragMargin);
    // The far corner stays on the map where a battlefield outgrows it.
    const int pointer_x =
        std::min(
            match_layout_.left + match_layout_.battlefield_width(),
            match_layout_.left + static_cast<int>(selected_tnt_->tile_width * 32U) - match_camera_x_
        ) -
        kDragEdgeInset;
    const int pointer_y =
        std::min(
            match_layout_.top + match_layout_.battlefield_height(),
            match_layout_.top + static_cast<int>(selected_tnt_->tile_height * 32U) - match_camera_z_
        ) -
        kDragEdgeInset;
    const auto before = frame;
    mouse(SDL_EVENT_MOUSE_MOTION, press_x, press_y);
    mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, press_x, press_y);
    if (!match_drag_)
        throw std::runtime_error(
            "selection check: a press on the battlefield at " + std::to_string(press_x) + ',' +
            std::to_string(press_y) + " started no drag box"
        );
    mouse(SDL_EVENT_MOUSE_MOTION, pointer_x, pointer_y);
    frame_of(frame);
    write_ppm(fs::path("local/reports") / "native-match-drag-box.ppm", frame);
    const auto [from, to] = match_drag_corners(viewport);
    if (std::abs(from.x - press_x) > kDragCornerSlack ||
        std::abs(from.y - press_y) > kDragCornerSlack ||
        std::abs(to.x - pointer_x) > kDragCornerSlack ||
        std::abs(to.y - pointer_y) > kDragCornerSlack)
        throw std::runtime_error(
            "selection check: the drag box runs " + std::to_string(from.x) + ',' +
            std::to_string(from.y) + " to " + std::to_string(to.x) + ',' + std::to_string(to.y) +
            ", not from the press to the pointer"
        );
    const CanvasRect cursor{
        pointer_x - kCursorReach, pointer_y - kCursorReach, 2 * kCursorReach, 2 * kCursorReach
    };
    std::size_t outline_pixels = 0;
    std::size_t outline_wrong = 0;
    int x1 = std::min(from.x, to.x), y1 = std::min(from.y, to.y);
    int x2 = std::max(from.x, to.x), y2 = std::max(from.y, to.y);
    for (const auto color : {uint8_t{15}, uint8_t{0}}) {
        const auto rgb = ui_color_rgb(color);
        for (const auto& [ax, ay, bx, by] :
             {std::array{x1, y1, x2, y1},
              std::array{x2, y1, x2, y2},
              std::array{x1, y2, x2, y2},
              std::array{x1, y1, x1, y2}})
            for (const auto& [x, y] : line_pixels(ax, ay, bx, by)) {
                if (contains(cursor, x, y))
                    continue;
                ++outline_pixels;
                outline_wrong += pixel_is(frame, x, y, rgb) ? 0 : 1;
            }
        ++x1;
        ++y1;
        --x2;
        --y2;
    }
    if (outline_pixels == 0 || outline_wrong != 0)
        throw std::runtime_error(
            "selection check: " + std::to_string(outline_wrong) + " of " +
            std::to_string(outline_pixels) + " drag box pixels are not UI colours 15 and 0"
        );
    mouse(SDL_EVENT_MOUSE_BUTTON_UP, pointer_x, pointer_y);
    if (match_drag_)
        throw std::runtime_error("selection check: the release kept the drag box");
    for (const auto id : group)
        if ((record(id).flags & OA_UNIT_FLAG_SELECTED) == 0)
            throw std::runtime_error(
                "selection check: the drag box left unit " + std::to_string(id) + " unselected"
            );
    frame_of(frame);
    write_ppm(fs::path("local/reports") / "native-match-selection-boxes.ppm", frame);
    // Each unit's box from the game's projection of its root bounds.
    const auto box_rgb = ui_color_rgb(static_cast<uint8_t>(wr::ui_color_rotated_box));
    const oa::sim::gameplay_input::Camera camera{match_camera_x_, match_camera_z_};
    const auto box_share = [&](const renderer::Surface& shown, uint16_t id) {
        const oa::Unit& unit = record(id);
        const auto [low, high] = oa::formats::objects3d::object_bounds(
            match_->instance(id)->model().model().objects.front()
        );
        const oa::formats::objects3d::FixedVector3 position{
            unit.position.x, unit.position.y, unit.position.z
        };
        const oa::sim::model_runtime::RotationWords rotation{
            unit.bank, static_cast<int16_t>(unit.heading), unit.pitch
        };
        std::array<std::array<int, 2>, 4> corners{};
        const oa::formats::objects3d::FixedVector3 box[4]{
            {low.x, low.y, low.z},
            {high.x, low.y, low.z},
            {high.x, low.y, high.z},
            {low.x, low.y, high.z}
        };
        for (std::size_t i = 0; i < 4; ++i) {
            const auto at = oa::sim::gameplay_input::project(
                oa::sim::model_runtime::rotate_vector(box[i], rotation), position, camera
            );
            corners[i] = {
                at.x - wr::battlefield_origin_x + match_layout_.left,
                at.y - wr::battlefield_origin_y + match_layout_.top
            };
        }
        std::size_t total = 0;
        std::size_t shown_pixels = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto& a = corners[i];
            const auto& b = corners[(i + 1) % 4];
            for (const auto& [x, y] : line_pixels(a[0], a[1], b[0], b[1])) {
                if (contains(cursor, x, y))
                    continue;
                ++total;
                shown_pixels += pixel_is(shown, x, y, box_rgb) ? 1 : 0;
            }
        }
        return std::pair{shown_pixels, total};
    };
    auto& game = match_->state().game;
    const auto options = oa::ui::console::console_flags(game);
    oa::ui::console::set_console_flags(
        game, static_cast<uint16_t>(options & ~oa::ui::console::console_flag::selection_boxes)
    );
    renderer::Surface unboxed;
    frame_of(unboxed);
    oa::ui::console::set_console_flags(game, options);
    std::size_t boxes = 0;
    for (const auto id : selected_local_ids()) {
        const auto& slot = slots[id];
        const auto at = project_match_point(viewport, slot.unit->position);
        if (!contains(
                {match_layout_.left,
                 match_layout_.top,
                 match_layout_.battlefield_width(),
                 match_layout_.battlefield_height()},
                at.x,
                at.y
            ) ||
            match_->instance(id) == nullptr)
            continue;
        const auto [selected, total] = box_share(frame, id);
        const auto [without, unused] = box_share(unboxed, id);
        const auto [unselected, ignored] = box_share(before, id);
        if (total == 0 || selected * 100U < total * kSelectionBoxMinPercent ||
            without * 2U > selected || unselected * 2U > selected)
            throw std::runtime_error(
                "selection check: unit " + std::to_string(id) + " shows " +
                std::to_string(selected) + " of " + std::to_string(total) +
                " selection box pixels (" + std::to_string(without) + " with SelBoxes off, " +
                std::to_string(unselected) + " unselected)"
            );
        ++boxes;
    }
    if (boxes < group.size())
        throw std::runtime_error("selection check: fewer boxes than selected units were checked");
    clear_local_selection();
    for (const auto id : saved_selection)
        adopt_selection(id);
    selected_match_unit_ = saved_primary;
    apply_match_hud_for_selection();
    match_command_ = saved_command;
    match_zoom_ = saved_zoom;
    match_zoom_target_ = saved_zoom_target;
    match_camera_x_ = saved_camera_x;
    match_camera_z_ = saved_camera_z;
    update_pointer(saved_pointer_x, saved_pointer_y);
    std::cout << "selection check: drag box " << outline_pixels << " pixels from " << press_x << ','
              << press_y << " to " << pointer_x << ',' << pointer_y << ", " << boxes
              << " selection boxes\n";
}

void Runtime::require_game_fonts(std::string_view screen, const std::function<void()>& draw) {
    if (!modern_fonts_open())
        return;
    const auto kept = engine_settings();
    auto game_fonts = kept;
    game_fonts.modern_fonts = false;
    auto modern = kept;
    modern.modern_fonts = true;
    modern.text_size = oa::present::highest_text_size;
    apply_engine_settings(game_fonts);
    draw();
    const auto expected = surface_;
    apply_engine_settings(modern);
    draw();
    const bool same = surface_.width == expected.width && surface_.height == expected.height &&
                      surface_.rgb == expected.rgb;
    apply_engine_settings(kept);
    draw();
    if (!same)
        throw std::runtime_error(std::string(screen) + " changed with the modern fonts on");
    std::cout << screen << " drew the same with the modern fonts on and off\n";
}

void Runtime::check_navigation() {
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    if (eligible_map_names_.empty()) {
        check_navigation_without_maps(report_directory);
        return;
    }
    const NavigationGroup group = options_.navigation_group;
    const auto runs = [group](NavigationGroup part) {
        return group == NavigationGroup::all || group == part;
    };
    const bool screens = runs(NavigationGroup::screens);
    const bool orders = runs(NavigationGroup::orders);
    const bool zoom = group == NavigationGroup::all || group == NavigationGroup::zoom ||
                      group == NavigationGroup::zoom_1366x768 ||
                      group == NavigationGroup::zoom_1920x1080 ||
                      group == NavigationGroup::zoom_2560x1440;
    if (!screens) {
        // Every other group starts from the skirmish menu, which the
        // screens group reaches through the Options screen.
        exercise_click(menu::resource_name(menu::Button::single_player));
        if (screen_ != Screen::single_player)
            throw std::runtime_error("navigation check did not reach SINGLE.GUI");
        exercise_click(entry::resource_name(entry::Button::skirmish));
        if (screen_ != Screen::skirmish)
            throw std::runtime_error("navigation check did not reach SKIRMISH.GUI");
    }
    int32_t capacity = 0;
    if (screens) {
        write_ppm(report_directory / "native-main.ppm", surface_);
        check_screen_services();
        load_progress_ = {100, 70, 40, 15, 0, 0};
        loading_flash_.fill(0);
        ensure_loading_screen();
        enter_loading_display();
        draw_loading_screen();
        write_ppm(report_directory / "native-loading.ppm", surface_);
        write_display_pcx(report_directory / "native-loading.pcx");
        // The loading screen and the menus' screens keep the game's own fonts
        // whatever the Language settings say.
        require_game_fonts("navigation check: the loading screen", [this] {
            draw_loading_screen();
        });
        exercise_click(menu::resource_name(menu::Button::single_player));
        if (screen_ != Screen::single_player || surface_.width != kCanvasWidth)
            throw std::runtime_error("navigation check did not reach SINGLE.GUI");
        write_ppm(report_directory / "native-single.ppm", surface_);
        exercise_click(entry::resource_name(entry::Button::options));
        if (screen_ != Screen::options || surface_.width != kCanvasWidth)
            throw std::runtime_error("navigation check did not reach STARTOPT.GUI");
        write_ppm(report_directory / "native-options.ppm", surface_);
        require_game_fonts("navigation check: the Options screen", [this] { rebuild_surface(); });
        exercise_click("CANCEL");
        if (screen_ != Screen::single_player)
            throw std::runtime_error("navigation check: CANCEL did not leave STARTOPT.GUI");
        check_options_gamma();
        exercise_click(entry::resource_name(entry::Button::skirmish));
        if (screen_ != Screen::skirmish || surface_.width != kCanvasWidth)
            throw std::runtime_error("navigation check did not reach SKIRMISH.GUI");
        write_ppm(report_directory / "native-skirmish.ppm", surface_);
    }
    // The match the screens and orders groups play in.
    uint16_t local_commander = 0;
    if (screens || orders) {
        state_.player_count = 2;
        capacity = map_player_capacity();
        if (capacity < 2)
            throw std::runtime_error("navigation check selected map lacks two start positions");
        exercise_click(skirmish::resource_name(skirmish::Button::start));
        if (screen_ != Screen::match || !match_)
            throw std::runtime_error("navigation check Start did not enter a real match");
        const auto armcom = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMCOM");
        const auto corcom = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "CORCOM");
        if (loaded_commander_types_.size() <= 1 || armcom == 0 || corcom == 0 ||
            !loaded_commander_types_[armcom].model || !loaded_commander_types_[corcom].model ||
            !loaded_commander_types_[armcom].script || !loaded_commander_types_[corcom].script)
            throw std::runtime_error("navigation check did not load commander runtimes");
        std::size_t commander_instances = 0;
        for (const auto& slot : match_->world().slots)
            if (slot.unit_index != 0 && slot.unit != nullptr &&
                match_->instance(slot.unit_index) != nullptr)
                ++commander_instances;
        if (commander_instances != 2)
            throw std::runtime_error("navigation check did not create both commanders");
        for (const auto& slot : match_->world().slots) {
            if (slot.unit_index != 0 && slot.unit != nullptr &&
                slot.record.owner_index == match_local_player_) {
                local_commander = slot.unit_index;
                break;
            }
        }
        if (local_commander == 0 || !match_->simulation().players[match_local_player_].present ||
            match_->simulation().players[match_local_player_].status != entry::controller::human)
            throw std::runtime_error("navigation check found inactive local simulation player");
    }
    if (screens) {
        auto& commander_slot = match_->world().slots[local_commander];
        const std::array<uint32_t, 3> start = commander_slot.unit->position;
        const oa::sim::ground_orders::Point bounded_destination{
            std::bit_cast<int32_t>(start[0]),
            std::bit_cast<int32_t>(start[1]),
            std::bit_cast<int32_t>(start[2]) + 64 * 65536
        };
        const uint32_t position_x_before = start[0];
        const uint32_t position_z_before = start[2];
        match_->issue_ground_move(local_commander, bounded_destination, false);
        match_timing_.tick = 1;
        match_->simulation().tick = match_timing_.tick;
        tick_or_raise(*match_);
        const auto* navigation = match_->ground_runtime(local_commander);
        if (navigation == nullptr || navigation->navigation.goal == nullptr)
            throw std::runtime_error("navigation check unit sweep skipped active commander");
        if (surface_.width != kCanvasWidth)
            throw std::runtime_error("navigation check did not render the match world");
        write_ppm(report_directory / "native-match.ppm", surface_);
        check_composed_frame();
        check_console_commands();
        check_game_speed_messages();
        for (int step = 0; step < 24; ++step) {
            ++match_timing_.tick;
            match_->simulation().tick = match_timing_.tick;
            tick_or_raise(*match_);
        }
        render_match_surface();
        write_ppm(report_directory / "native-match-moved.ppm", surface_);
        if (commander_slot.unit->position[0] == position_x_before &&
            commander_slot.unit->position[2] == position_z_before)
            throw std::runtime_error(
                "navigation check commander did not move toward issued destination"
            );
        uint16_t enemy_commander = 0;
        for (const auto& slot : match_->world().slots) {
            if (slot.unit_index != 0 && slot.unit != nullptr &&
                slot.owner_index != match_local_player_) {
                enemy_commander = slot.unit_index;
                break;
            }
        }
        if (enemy_commander != 0) {
            try {
                if (!match_->issue_attack(local_commander, enemy_commander, false))
                    std::cerr << "navigation check attack: refused\n";
            } catch (const std::exception& error) {
                std::cerr << "navigation check attack: " << error.what() << '\n';
            }
            for (int step = 0; step < 30; ++step) {
                ++match_timing_.tick;
                match_->simulation().tick = match_timing_.tick;
                tick_or_raise(*match_);
            }
            render_match_surface();
            write_ppm(report_directory / "native-match-fire.ppm", surface_);
        }
        const auto solar = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMSOLAR");
        if (solar != 0) {
            const std::array<uint32_t, 3> at = commander_slot.unit->position;
            const oa::sim::ground_orders::Point yard{
                std::bit_cast<int32_t>(at[0]) + 64 * 65536,
                std::bit_cast<int32_t>(at[1]),
                std::bit_cast<int32_t>(at[2])
            };
            try {
                match_->issue_mobile_build(local_commander, solar, yard, false);
            } catch (const std::exception& error) {
                std::cerr << "navigation check build: " << error.what() << '\n';
            }
            bool wrote_nano = false;
            for (int step = 0; step < 180; ++step) {
                ++match_timing_.tick;
                match_->simulation().tick = match_timing_.tick;
                try {
                    tick_or_raise(*match_);
                } catch (const std::exception& error) {
                    std::cerr << "navigation check build tick: " << error.what() << '\n';
                    break;
                }
                if (!wrote_nano &&
                    (!match_->nano_lasers().empty() || match_->particle_count() != 0)) {
                    render_match_surface();
                    write_ppm(report_directory / "native-match-nano.ppm", surface_);
                    wrote_nano = true;
                }
            }
            if (!wrote_nano) {
                render_match_surface();
                write_ppm(report_directory / "native-match-nano.ppm", surface_);
            }
        }
    }
    if (orders) {
        check_builder_orders();
        check_build_placement();
        check_selection_visuals([this](renderer::Surface& frame) {
            render_match_surface();
            compose_match_frame(frame);
        });
        show_match_build_page(1);
        write_ppm(report_directory / "native-match-build.ppm", surface_);
        // The Pause key, the menus' hold, the team panels and the keys the
        // paused panels take, over this game, which the victory check below
        // leaves.
        check_pause_key();
        check_team_panels();
        check_match_panel_keys();
    }
    if (runs(NavigationGroup::outcomes)) {
        // The console check's Kill left this game without victory or defeat, so
        // the victory is played in a new one.
        return_to_skirmish_menu();
        check_skirmish_victory(report_directory);
        check_deathmatch_respawn();
        check_dgun_order();
        check_attack_command();
    }
    if (zoom)
        check_tracking_zoom(group);
    if (runs(NavigationGroup::outcomes))
        check_turret_draws(report_directory);
    if (!runs(NavigationGroup::campaign))
        return;
    check_launch_services();
    exercise_click(skirmish::resource_name(skirmish::Button::select_map));
    if (screen_ != Screen::map_selection || bound_map_names_.empty())
        throw std::runtime_error("navigation check did not open the map selection");
    preview_map_index(bound_map_names_.size() - 1U);
    const auto first_visible = map_first_visible();
    const auto* map_list = widget("MAPNAMES");
    if (map_list == nullptr)
        throw std::runtime_error("map list widget is absent");
    const auto modal_offset_y =
        (kCanvasHeight - static_cast<int>(resources_.layout.gadgets.front().common.height)) / 2;
    select_map_row_at(static_cast<float>(modal_offset_y + map_list->common.y + 3));
    if (static_cast<std::size_t>(modal_map_index_) != first_visible)
        throw std::runtime_error("map list row did not update the selected preview");
    state_.player_count = 2;
    const auto alternate_capacity = map_player_capacity();
    if (alternate_capacity < 2 || !map_picture_ || map_picture_->rgb.empty())
        throw std::runtime_error("map selection preview lacks terrain or start positions");
    rebuild_surface();
    write_ppm(report_directory / "native-map-select.ppm", surface_);
    const auto committed_map = bound_map_names_[static_cast<std::size_t>(modal_map_index_)];
    exercise_click(map_modal::resource_name(map_modal::Button::load));
    close_map_modal();
    if (screen_ != Screen::skirmish || skirmish_settings_.map_name != committed_map)
        throw std::runtime_error("map Load did not commit and restore skirmish");
    exercise_click(skirmish::resource_name(skirmish::Button::select_map));
    exercise_click(map_modal::resource_name(map_modal::Button::previous_menu));
    close_map_modal();
    if (screen_ != Screen::skirmish || skirmish_settings_.map_name != committed_map)
        throw std::runtime_error("map Cancel changed selection or failed to restore parent");
    std::cout << "navigation check: MAINMENU.GUI -> SINGLE.GUI -> SKIRMISH.GUI\n";
    if (capacity != 0)
        std::cout << "match bootstrap check: " << capacity << " OTA positions, "
                  << loaded_commander_types_.size() - 1U
                  << " unit runtimes including ARMCOM/CORCOM\n";
    std::cout << "map selection check: " << bound_map_names_.size()
              << " eligible maps, preview capacity " << alternate_capacity << '\n';
    preferences_.side = 0;
    preferences_.difficulty = 0;
    show_mission_briefing();
    write_ppm(report_directory / "native-briefing.ppm", surface_);
    if (screen_ != Screen::briefing)
        throw std::runtime_error("campaign Start did not open MSNBRIEF.GUI");
    start_campaign_mission();
    if (screen_ != Screen::match || !match_ || !campaign_mission_)
        throw std::runtime_error("navigation check campaign Start did not enter the mission");
    check_player_records("campaign");
    std::size_t placed = 0;
    for (const auto& slot : match_->world().slots)
        if (slot.unit != nullptr && slot.unit->type_index)
            ++placed;
    if (placed < 8)
        throw std::runtime_error("navigation check campaign placed too few units");
    render_match_surface();
    write_ppm(report_directory / "native-campaign.ppm", surface_);
    std::cout << "campaign check: " << selected_map_name_runtime_ << " placed " << placed
              << " units\n";
    check_campaign_build_page(report_directory);
    const auto in_match = check_save_dialog(report_directory, "NAVMATCH");
    if (in_match.game_type != oa::data::persist::game_type_campaign || in_match.between_missions ||
        std::string_view(in_match.mission.data()) != bound_mission_name())
        throw std::runtime_error("the in-match save is not the running campaign mission");
    match_outcome_ = sim::scenario::Outcome::victory;
    match_->state().game.outcome_flags |= sim::scenario::outcome_flag::won;
    match_finished_ = true;
    finish_match_outcome();
    if (screen_ != Screen::campaign_end || resources_.layout.gadgets.size() < 4)
        throw std::runtime_error("ENDMSN.GUI did not load campaign result gadgets");
    check_endgame_screen(report_directory);
    write_ppm(report_directory / "native-campaign-end.ppm", surface_);
    require_game_fonts("navigation check: the mission's statistics", [this] { rebuild_surface(); });
    check_campaign_advance(report_directory);
    const auto between = check_save_dialog(report_directory, "NAVBETWEEN", true);
    if (!between.between_missions ||
        std::string_view(between.mission.data()) == bound_mission_name() ||
        endgame_world() == nullptr)
        throw std::runtime_error("the ENDMSN save does not name the next mission between missions");
    check_end_panel_load_cancel();
    load(Screen::new_campaign);
    write_ppm(report_directory / "native-newcamp.ppm", surface_);
    // The installs carry more than two campaign files, so the
    // screen lists the campaigns over newcampaign4.
    const auto* campaign_list = widget("Campaign");
    if (campaign_labels_.empty() || campaign_list == nullptr || campaign_list->common.active == 0 ||
        std::string_view(frontend_game().background_name) != "newcampaign4")
        throw std::runtime_error("new campaign did not list the campaigns over newcampaign4");
    std::cout << "new campaign check: campaign list over newcampaign4\n";
    load(Screen::any_mission);
    write_ppm(report_directory / "native-anymsn.ppm", surface_);
    if (campaign_files_.empty() || campaign_mission_files_.size() < 20)
        throw std::runtime_error("any mission did not list campaign missions");
    std::cout << "any mission check: " << campaign_labels_.size() << " campaigns, "
              << campaign_mission_files_.size() << " missions\n";
    check_dialogs(report_directory);
}

void Runtime::check_screen_services() {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("screen service check: " + what);
    };
    require(screen_ == Screen::main_menu && !match_, "needs the main menu with no match");
    auto* setup = setup_step_binding(screens_);
    require(setup != nullptr, "the main menu's setup step has no binding");
    auto& probe = service_probe();
    probe = {};
    probe.setup_step = *setup;
    // What the check changes, put back at its end.
    const Extension saved_extension = extension_;
    const StepDesc saved_setup = *setup;
    const uint32_t saved_query_count = screens_.query_count;
    const auto saved_values = preference_values_;
    const bool saved_dirty = preferences_dirty_;
    const auto restore = [&] {
        extension_ = saved_extension;
        *setup = saved_setup;
        screens_.query_count = saved_query_count;
        exit_requested_ = false;
        exit_status_ = 0;
        quit_requested_ = false;
        quit_reason_.clear();
        frontend_pass_requested_ = false;
        // The preferences load again from the stored values, with the
        // extension's own launch values.
        preference_values_ = saved_values;
        step(frontend::Step::load_preferences, state_);
        preferences_dirty_ = saved_dirty;
    };
    const auto context = screen_context();
    const auto& services = *context.services;
    try {
        // A close request the extension answers leaves the run going; one it
        // declines, or no hook, ends it at once on the main menu.
        SDL_Event close{};
        close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        bool running = true;
        extension_.close_requested = probe_close_requested;
        probe.close_answer = true;
        handle_sdl_event(close, running);
        require(
            running && !exit_requested_ && screen_ == Screen::main_menu &&
                probe.close_requests == 1,
            "a close request the extension answered ended the run"
        );
        probe.close_answer = false;
        handle_sdl_event(close, running);
        require(
            !running && probe.close_requests == 2,
            "a close request the extension declined did not end the run on the main menu"
        );
        if (sdl_.window == nullptr) {
            // Without a window the system's quit is not offered.
            SDL_Event quit_event{};
            quit_event.type = SDL_EVENT_QUIT;
            running = true;
            handle_sdl_event(quit_event, running);
            require(
                !running && probe.close_requests == 2,
                "the system's quit without a window was offered to the extension"
            );
        }
        extension_.close_requested = nullptr;
        running = true;
        handle_sdl_event(close, running);
        require(!running, "a close request without the hook did not end the run");

        // quit ends the run with its status once the callback has returned;
        // the reason goes to standard error without a window.
        services.quit(context.host, "the reason the screen service check gives quit", 5);
        require(!exit_requested_, "quit ended the run inside the callback");
        apply_screen_request();
        require(
            exit_requested_ && exit_status_ == 5 && screen_ == Screen::main_menu &&
                !quit_requested_,
            "quit did not end the run with its status"
        );
        exit_requested_ = false;
        exit_status_ = 0;

        // The sound stops and the alternate route; a headless run plays
        // nothing on it.
        services.stop_sounds(context.host);
        require(
            !menu_music_playing_ && !audio_player_.playing(), "stop_sounds left a sound playing"
        );
        services.play_sound_alternate(context.host, "BGM");
        services.play_sound_alternate(context.host, nullptr);
        require(!menu_music_playing_, "the alternate route played in a headless run");

        // Two requests make one pass of the dispatcher, once the callback has
        // returned: the main menu's initialize runs its setup again.
        setup->run = probe_setup_step;
        services.set_frontend_state(context.host, frontend::state_id::main_menu);
        services.set_frontend_signal(context.host, frontend::signal_id::initialize);
        services.run_frontend(context.host);
        services.run_frontend(context.host);
        require(probe.setup_runs == 0, "run_frontend ran the dispatcher at once");
        apply_screen_request();
        require(
            probe.setup_runs == 1 && !frontend_pass_requested_ && screen_ == Screen::main_menu,
            "two requests did not make one pass that set the main menu up again"
        );
        apply_screen_request();
        require(probe.setup_runs == 1, "a frontend pass ran without a request");

        // A query binding answers through Runtime::query; an unbound query
        // answers 0.
        // A label no dispatcher state asks, and what its probe answers.
        const auto spare_query = static_cast<frontend::Query>(0x2ff);
        constexpr uint32_t kProbeAnswer = 0x5eed;
        probe.query_value = kProbeAnswer;
        require(query(spare_query, state_) == 0, "an unbound query did not answer 0");
        require(
            query_register(&screens_, spare_query, probe_query, nullptr),
            "the probe query was not registered"
        );
        require(
            query(spare_query, state_) == kProbeAnswer,
            "a bound query did not answer the dispatcher"
        );

        // The preferences load takes the launch's nickname and game name, and
        // asks for them once.
        extension_.frontend_entry = probe_frontend_entry;
        probe.nickname = "Visitor";
        probe.game_name = "Room";
        step(frontend::Step::load_preferences, state_);
        require(
            preferences_.nickname == "Visitor" && preferences_.game_name == "Room" &&
                probe.entry_calls == 1,
            "the preferences load did not take the launch's nickname and game name once"
        );
        probe.nickname = "NicknameOfTwentyChar";
        step(frontend::Step::load_preferences, state_);
        require(preferences_.nickname == "NicknameOfTwenty", "a long nickname was not cut to 16");
        write_string(init::general_section, "Nickname", "Stored");
        probe.nickname = nullptr;
        step(frontend::Step::load_preferences, state_);
        require(
            preferences_.nickname == "Stored" && preferences_.game_name == "Room",
            "a launch without a nickname did not keep the stored one"
        );
        probe.nickname = "";
        step(frontend::Step::load_preferences, state_);
        require(preferences_.nickname == "Stored", "an empty nickname replaced the stored one");
    } catch (...) {
        restore();
        throw;
    }
    restore();
    std::cout << "screen service check: close requests answered and declined, quit's status, "
                 "sound stops, one frontend pass for two requests, a query binding and the "
                 "launch's nickname\n";
}

void Runtime::check_launch_services() {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("launch service check: " + what);
    };
    require(screen_ == Screen::skirmish, "needs the skirmish menu");
    auto* setup = setup_step_binding(screens_);
    require(setup != nullptr, "the main menu's setup step has no binding");
    auto& probe = service_probe();
    probe = {};
    probe.setup_step = *setup;
    const Extension saved_extension = extension_;
    const StepDesc saved_setup = *setup;
    probe.match_event = saved_extension.match_event;
    const auto restore = [&] {
        if (match_ && match_paused_ && !match_finished_)
            resume_match_pause();
        extension_ = saved_extension;
        *setup = saved_setup;
        exit_requested_ = false;
        exit_status_ = 0;
        quit_requested_ = false;
        quit_reason_.clear();
        frontend_pass_requested_ = false;
    };
    constexpr std::size_t kLabelLength = oa::ui::frontend::kReturnLabelBytes - 1U;
    try {
        // The match start keeps the return label.
        extension_.return_label = probe_return_label;
        extension_.match_event = probe_match_event;
        probe.label = "Portal";
        exercise_click(skirmish::resource_name(skirmish::Button::start));
        require(screen_ == Screen::match && match_, "Start did not enter a match");
        require(
            std::string_view(return_label_.data()) == "Portal",
            "the match start did not keep the return label"
        );
        // A label longer than the menus take is cut; none leaves it empty.
        const std::string long_label(kLabelLength + 9U, 'L');
        probe.label = long_label.c_str();
        take_return_label();
        require(
            ::strnlen(return_label_.data(), return_label_.size()) == kLabelLength,
            "a long label was not cut to the menus' length"
        );
        probe.label = nullptr;
        take_return_label();
        require(return_label_[0] == '\0', "no label left one kept");

        // A close request the extension answers asks nothing in the match; one
        // it declines asks whether to surrender.
        SDL_Event close{};
        close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        bool running = true;
        const auto panel = match_hud_panel_;
        extension_.close_requested = probe_close_requested;
        probe.close_answer = true;
        handle_sdl_event(close, running);
        require(
            running && !exit_requested_ && !match_paused_ && match_hud_panel_ == panel &&
                probe.close_requests == 1,
            "a close request the extension answered asked in the match"
        );
        probe.close_answer = false;
        handle_sdl_event(close, running);
        require(
            running && match_paused_ &&
                match_hud_panel_ == oa::data::defs::gui_path("YESORNO.GUI") &&
                probe.close_requests == 2,
            "a close request the extension declined did not ask to surrender"
        );
        escape_match_menu();
        require(!match_paused_, "Escape did not close the surrender confirmation");

        // A frontend pass requested in a match does not run, and is dropped.
        const auto context = screen_context();
        setup->run = probe_setup_step;
        context.services->run_frontend(context.host);
        apply_screen_request();
        require(
            probe.setup_runs == 0 && !frontend_pass_requested_ && screen_ == Screen::match,
            "a frontend pass ran in a match"
        );

        // quit leaves the running match first, as the exit confirmation's
        // first choice does, once the callback has returned; the preferences
        // open over it close with it, and the next match opens with none.
        show_match_pause_menu();
        activate_pause_gadget("PREFS");
        require(
            match_preferences_open() && match_hud_panel_ == oa::data::defs::gui_path("PREFS.GUI"),
            "PREFS did not open the preferences over the match"
        );
        context.services->quit(context.host, nullptr, 7);
        require(
            match_ && !exit_requested_ && probe.left_events == 0,
            "quit left the match inside the callback"
        );
        apply_screen_request();
        require(
            exit_requested_ && exit_status_ == 7 && !match_ && probe.left_events == 1,
            "quit in a match did not leave it before ending the run"
        );
        require(
            !match_preferences_open() && options_parent_ != Screen::match,
            "the preferences open as the match went stayed open for the next match"
        );
    } catch (...) {
        restore();
        throw;
    }
    restore();
    return_to_skirmish_menu();
    std::cout << "launch service check: the return label kept at the match start and cut to "
              << kLabelLength
              << " characters, close requests in the match, no frontend pass there, and quit "
                 "leaving the match first with its preferences\n";
}

void Runtime::return_to_skirmish_menu() {
    match_finished_ = false;
    match_paused_ = false;
    match_outcome_ = sim::scenario::Outcome::ongoing;
    screen_ = Screen::skirmish;
    state_.state = frontend::state_id::skirmish_menu;
    rebuild_surface();
}

void Runtime::check_skirmish_victory(const fs::path& report_directory) {
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    if (screen_ != Screen::match || !match_)
        throw std::runtime_error("victory check: Start did not enter a match");
    const oa::World& world = match_->state();
    std::size_t swept = 0;
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        if (player != match_local_player_ && world.game.players[player].unit_count != 0) {
            match_->destroy_player_units(player);
            ++swept;
        }
    }
    if (swept == 0)
        throw std::runtime_error("victory check: no opponent has a unit to sweep");
    for (uint32_t step = 0; step < kVictoryTickLimit && !match_finished_; ++step) {
        ++match_timing_.tick;
        match_->simulation().tick = match_timing_.tick;
        tick_or_raise(*match_);
        present_match_outcome();
    }
    if (!match_finished_ || match_->outcome() != sim::scenario::Outcome::victory)
        throw std::runtime_error("navigation check did not present skirmish victory");
    render_match_surface();
    write_ppm(report_directory / "native-match-victory.ppm", surface_);
    std::cout << "skirmish victory check: opponents swept " << swept
              << ", victory presented at tick " << match_timing_.tick << '\n';
    return_to_skirmish_menu();
}

void Runtime::check_campaign_advance(const fs::path& report_directory) {
    const auto* world = endgame_world();
    if (world == nullptr)
        throw std::runtime_error("ENDMSN kept no finished game");
    const auto finished = static_cast<std::size_t>(world->game.mission_index);
    if (finished >= frontend::start_pattern_bytes || state_.mission_results[finished] != 'W')
        throw std::runtime_error("ENDMSN did not record the won mission");
    if (selected_mission_index_ != finished + 1)
        throw std::runtime_error("ENDMSN did not preselect the next mission");
    exercise_click("Start");
    if (screen_ != Screen::briefing || bound_mission_index() != static_cast<int32_t>(finished + 1))
        throw std::runtime_error("ENDMSN Start did not open the next mission's briefing");
    write_ppm(report_directory / "native-campaign-next-briefing.ppm", surface_);
    const auto next = bound_mission_name();
    exercise_click("PrevMenu");
    if (screen_ != Screen::campaign_end || endgame_world() == nullptr ||
        selected_mission_index_ != finished + 1)
        throw std::runtime_error(
            "the next mission's briefing did not return to ENDMSN (screen " +
            std::to_string(static_cast<int>(screen_)) + ", kept game " +
            (endgame_world() != nullptr ? "yes" : "no") + ", selected mission " +
            std::to_string(selected_mission_index_) + ")"
        );
    std::cout << "campaign advance check: ENDMSN Start -> " << next << " briefing -> ENDMSN\n";
}

void Runtime::check_campaign_build_page(const fs::path& report_directory) {
    const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMCOM");
    if (type == 0)
        throw std::runtime_error("the campaign's use-only list has no ARMCOM");
    uint16_t anchor = 0;
    for (const auto& slot : match_->world().slots)
        if (slot.unit_index != 0 && slot.unit != nullptr &&
            slot.record.owner_index == match_local_player_) {
            anchor = slot.unit_index;
            break;
        }
    if (anchor == 0)
        throw std::runtime_error("the campaign mission has no local unit");
    const auto& beside = match_->world().slots[anchor];
    const auto x = static_cast<uint32_t>(beside.unit->position[0] >> 16);
    const auto z = static_cast<uint32_t>(beside.unit->position[2] >> 16) + 64U;
    oa::sim::unit_spawn::Request request;
    request.player = match_local_player_;
    request.type = type;
    request.finished = true;
    request.state = kGroundOccupancyState;
    request.position = {
        x << 16, static_cast<uint32_t>(match_->map_height(x << 16, z << 16)) << 16, z << 16
    };
    auto* spawned = match_->create(request);
    if (spawned == nullptr || spawned->unit == nullptr)
        throw std::runtime_error("the build page check could not place an ARMCOM");
    spawned->unit->object_present = true;
    const auto commander = spawned->unit_index;
    const auto previous = selected_match_unit_;
    selected_match_unit_ = commander;
    show_match_build_page(1);
    std::size_t unit_buttons = 0;
    std::size_t greyed = 0;
    for (const auto& gadget : match_hud_->layout.gadgets) {
        const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
        if (button == nullptr || (static_cast<uint8_t>(gadget.common.common_attributes) &
                                  oa::ui::hud::kCommonUnitButton) == 0)
            continue;
        ++unit_buttons;
        const bool available =
            oa::sim::unit_spawn::find_type_index(spawn_type_names_, gadget.common.name) != 0;
        if (button->grayed_out == available)
            throw std::runtime_error(
                "build button " + gadget.common.name + " does not follow the use-only list"
            );
        if (!available) {
            ++greyed;
            if (gadget_command_available(gadget) || !greyed_picture_frame(gadget))
                throw std::runtime_error(
                    "greyed build button " + gadget.common.name + " is not drawn disabled"
                );
        }
    }
    if (greyed == 0)
        throw std::runtime_error("the use-only list greyed no build button");
    write_ppm(report_directory / "native-campaign-build.ppm", surface_);
    std::cout << "campaign build page check: " << greyed << " of " << unit_buttons
              << " unit buttons greyed\n";
    selected_match_unit_ = previous;
    apply_match_hud_for_selection();
}

oa::ui::frontend::LoadSummary Runtime::check_save_dialog(
    const fs::path& report_directory, const std::string& name, bool compose
) {
    const auto parent = screen_;
    if (parent == Screen::match) {
        show_match_pause_menu();
        activate_pause_gadget("SAVEGAME");
    } else if (const auto* save = widget("SaveGame"); save != nullptr && save->common.active != 0) {
        click_end_panel(static_cast<std::size_t>(save - resources_.layout.gadgets.data()));
    }
    if (screen_ != Screen::load_game || !save_dialog_open())
        throw std::runtime_error("SAVEGAME did not open the save dialog: " + status_);
    tick_screen_packages();
    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.key = SDLK_BACKSPACE;
    // The typed name below is checked; the erasing keys need not be taken
    // once the field is empty.
    for (int erase = 0; erase < 32; ++erase)
        std::ignore = dispatch_screen_input(key);
    SDL_Event text{};
    text.type = SDL_EVENT_TEXT_INPUT;
    text.text.text = name.c_str();
    if (!dispatch_screen_input(text))
        throw std::runtime_error("the save dialog took no typed name");
    std::string saved_name = name;
    const auto kept_settings = engine_settings();
    if (compose) {
        // U+5B58 U+6863, composed from their spelling and committed; U+5B57
        // typed after them and erased.
        constexpr std::string_view composed = "\xE5\xAD\x98\xE6\xA1\xA3";
        constexpr std::string_view erased = "\xE5\xAD\x97";
        bool running = true;
        rebuild_surface();
        const renderer::Surface name_alone = surface_;
        // While the game's text is in its code page the name takes printable
        // ASCII alone: neither U+00E9 nor U+5B57 typed now is taken, and the
        // dialog looks as it did.
        const std::string refused = "\xC3\xA9" + std::string(erased);
        text.text.text = refused.c_str();
        dispatch_event(text, running);
        rebuild_surface();
        if (surface_.rgb != name_alone.rgb)
            throw std::runtime_error("the save dialog's name took a character past ASCII");
        // With Unicode chat on, the game's text is UTF-8 and the name takes
        // any script, until the save is written.
        auto unicode = kept_settings;
        unicode.unicode_chat = true;
        apply_engine_settings(unicode);
        SDL_Event editing{};
        editing.type = SDL_EVENT_TEXT_EDITING;
        editing.edit.text = "cundang";
        dispatch_event(editing, running);
        // The input method's keys: neither erases the name nor saves it.
        for (const SDL_Keycode composing : {SDLK_BACKSPACE, SDLK_RETURN}) {
            key.key.key = composing;
            dispatch_event(key, running);
        }
        const std::string composition(composed);
        editing.edit.text = composition.c_str();
        dispatch_event(editing, running);
        if (screen_ != Screen::load_game || !save_dialog_open())
            throw std::runtime_error("a key pressed while composing closed the save dialog");
        rebuild_surface();
        write_ppm(report_directory / ("native-save-" + name + "-composing.ppm"), surface_);
        const renderer::Surface composing = surface_;
        text.text.text = composition.c_str();
        dispatch_event(text, running);
        // The composition is underlined in the dialog's letter colour on one
        // row under its characters; committed, the characters stay as they
        // were and the underline goes.
        rebuild_surface();
        std::set<std::size_t> marked_rows;
        bool letter_colour = true;
        for (std::size_t at = 0; at + 2 < surface_.rgb.size() && at + 2 < composing.rgb.size();
             at += 3) {
            const auto shown = composing.rgb.begin() + static_cast<std::ptrdiff_t>(at);
            if (std::equal(
                    shown, shown + 3, surface_.rgb.begin() + static_cast<std::ptrdiff_t>(at)
                ))
                continue;
            marked_rows.insert(at / 3 / surface_.width);
            letter_colour =
                letter_colour &&
                std::equal(
                    oa::present::gui_font_color.begin(), oa::present::gui_font_color.end(), shown
                );
        }
        if (marked_rows.size() != 1 || !letter_colour)
            throw std::runtime_error(
                "the save dialog did not underline the input method's composition"
            );
        const std::string extra(erased);
        text.text.text = extra.c_str();
        dispatch_event(text, running);
        key.key.key = SDLK_BACKSPACE;
        dispatch_event(key, running);
        saved_name += composed;
        // The modern fonts draw the hanzi: the name looks otherwise than its
        // bytes drawn in the dialog's font, as they are while the game's text
        // is in its code page.
        rebuild_surface();
        const renderer::Surface in_utf8 = surface_;
        apply_engine_settings(kept_settings);
        rebuild_surface();
        if (modern_fonts_open() && surface_.rgb == in_utf8.rgb)
            throw std::runtime_error("the save dialog drew the name's hanzi as its font's bytes");
        apply_engine_settings(unicode);
    }
    rebuild_surface();
    write_ppm(report_directory / ("native-save-" + name + ".ppm"), surface_);
    key.key.key = SDLK_RETURN;
    // What Return did is checked below.
    std::ignore = dispatch_screen_input(key);
    if (compose)
        apply_engine_settings(kept_settings);
    if (screen_ != parent || save_dialog_open())
        throw std::runtime_error(
            "the save did not return to the screen it was opened over: " + status_
        );
    // The save is listed under the name typed, and read back.
    fs::path written;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(saves_folder(), error))
        if (tdf_names_equal(path_to_utf8(entry.path().stem()), saved_name))
            written = entry.path();
    oa::ui::frontend::LoadSummary summary;
    if (written.empty() || !read_save_summary(written, summary))
        throw std::runtime_error("the save dialog wrote no readable " + saved_name);
    std::cout << "save dialog check: " << path_to_utf8(written.filename()) << " mission "
              << summary.mission.data() << (summary.between_missions ? " between missions" : "")
              << '\n';
    return summary;
}

} // namespace oa::app
