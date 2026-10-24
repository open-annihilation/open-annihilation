// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The in-game console's developer displays and crash tests: the debug grid
// and "Contour" lines over the terrain, the "Profile" frame-time bars, the
// debug keys' frame-rate line and "DebugBreak", with their headless check.
#include "oa/app/runtime.hpp"
#include "match_models.hpp"
#include "oa/app/match_console.hpp"

#include "oa/present/model/model_library.hpp"
#include "oa/present/model/rgb_bridge.hpp"
#include "oa/platform/crash.hpp"
#include "oa/present/display.hpp"
#include "oa/present/polygon.hpp"
#include "oa/present/raster.hpp"
#include "oa/sim/profile.hpp"
#include "oa/sim/messages.hpp"
#include "oa/sim/selection.hpp"
#include "oa/ui/console/console.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/services/timers.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/present/world_renderer/world_overlays.hpp"
#include "match_fault.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>

namespace oa::app {

namespace console = oa::ui::console;
namespace wr = oa::present::world_renderer;
namespace profile = oa::sim::profile;

namespace {

// The crash tests wait this long after taking a fullscreen display back to a
// window, before it breaks.
constexpr uint32_t kDebugBreakSettleMs = 500;
// The debug keys' line on the 640x480 screen (draw_debug_status_line): its
// row lies three COMIX line heights less ten pixels down, and its three
// parts start at these columns.
constexpr int32_t kDebugLineRows = 3;
constexpr int32_t kDebugLineRaise = 10;
constexpr int32_t kDebugLineRateX = 131;
constexpr int32_t kDebugLineBuildX = 188;
constexpr int32_t kDebugLineModeX = 494;
// The build the line names, and the mode words: the debug keys are on
// whenever the line is drawn, and debug key 'i' turns the information on.
constexpr const char* kDebugLineBuild = "[Release]";
constexpr const char* kDebugLineMode = "DEBUG";
constexpr const char* kDebugLineInfoOn = "ON";
constexpr const char* kDebugLineInfoOff = "OFF";
// The debug grid's font, smlfont, which the grid reads on its first draw
// (start-up keeps its own smlfont, with COMIX, in place of Game.common_fonts).
constexpr const char* kSmallFontPath = "fonts\\smlfont.FNT";

// The overlays place the battlefield at (0x80, 0x20); the bridge's 8-bit
// view starts at that corner.
int32_t view_x(int32_t x) {
    return x - wr::battlefield_origin_x;
}

int32_t view_y(int32_t y) {
    return y - wr::battlefield_origin_y;
}

wr::OverlayRaster battlefield_raster() {
    wr::OverlayRaster raster = wr::present_overlay_raster();
    // The debug overlay is drawn again every frame, so whether a shape was
    // drawn does not matter.
    raster.line = [](void*,
                     oa::Surface* surface,
                     int32_t x0,
                     int32_t y0,
                     int32_t x1,
                     int32_t y1,
                     uint8_t color) {
        std::ignore = oa::present::draw_clipped_line(
            surface, view_x(x0), view_y(y0), view_x(x1), view_y(y1), color
        );
    };
    raster.fill_rect = [](void*, oa::Surface* surface, const oa::Rect32& rect, uint8_t color) {
        std::ignore = oa::present::fill_clipped_rect(
            surface, {view_x(rect.x1), view_y(rect.y1), view_x(rect.x2), view_y(rect.y2)}, color
        );
    };
    raster.polygon =
        [](void*, oa::Surface* surface, const int32_t* points, int32_t count, uint8_t color) {
            std::array<oa::present::PolygonVertex, 4> vertices{};
            count = std::min<int32_t>(count, static_cast<int32_t>(vertices.size()));
            for (int32_t i = 0; i < count; ++i)
                vertices[static_cast<std::size_t>(i)] = {
                    view_x(points[i * 2]), view_y(points[i * 2 + 1])
                };
            std::ignore = oa::present::fill_polygon(surface, vertices.data(), count, color);
        };
    raster.text = [](void*, oa::Surface* surface, const char* text, int32_t x, int32_t y) {
        oa::present::draw_text(
            surface, text, view_x(x), view_y(y), oa::present::text_width_unbounded
        );
    };
    return raster;
}

// What the search view reads for one frame: the movement map of the local
// player's next selected unit and the path search's cell map.
struct DebugGridView {
    oa::sim::match_runtime::Match* match{};
    const std::vector<oa::sim::match_runtime::RuntimeTypeFields>* fields{};
    const oa::sim::ground_orders::MovementMap* movement{};
};

} // namespace

void Runtime::draw_match_debug_grid(
    oa::present::model::RgbBridge& bridge,
    oa::present::model::ModelDisplay& display,
    const oa::present::model::RgbFrame& frame,
    const oa::Rect32& area,
    float scale
) {
    if (!match_)
        return;
    auto& world = match_->state();
    const oa::Game& game = world.game;
    const int32_t* contour = console_ ? console_->state.contour_values : nullptr;
    if (game.debug_overlay == wr::debug_view::off && (contour == nullptr || contour[0] == 0))
        return;
    if (debug_font_.empty()) {
        try {
            debug_font_ = assets_.read(kSmallFontPath).bytes;
        } catch (const std::exception& error) {
            std::cerr << "debug grid font unavailable: " << error.what() << '\n';
        }
    }
    DebugGridView view{match_.get(), &offline_type_fields_, nullptr};
    wr::DebugGridSources sources;
    sources.plots = world.plots;
    if (game.viewpoint_player < OA_PLAYER_COUNT) {
        const auto coverage = match_->player_coverage(game.viewpoint_player);
        sources.coverage = coverage.empty() ? nullptr : coverage.data();
    }
    sources.small_font = debug_font_.empty() ? nullptr : debug_font_.data();
    if (contour != nullptr) {
        sources.contour_spacing = contour[0];
        sources.contour_phase = contour[1];
    }
    sources.user = &view;
    if (live_renderer_active()) {
        sources.projection_user = this;
        sources.project = [](void* user,
                             int32_t x,
                             int32_t height,
                             int32_t z,
                             int32_t& screen_x,
                             int32_t& screen_y) {
            const auto& runtime = *static_cast<Runtime*>(user);
            const auto p = runtime.live_project_world(
                static_cast<float>(x), static_cast<float>(height), static_cast<float>(z)
            );
            screen_x = p.x - runtime.match_layout_.left + wr::battlefield_origin_x;
            screen_y = p.y - runtime.match_layout_.top + wr::battlefield_origin_y;
        };
    }

    sources.open_movement_class = [](void* user) {
        auto& v = *static_cast<DebugGridView*>(user);
        const oa::Unit* unit =
            oa::sim::selection::next_selected_unit(v.match->state(), nullptr, false);
        if (unit == nullptr || unit->type_index >= v.fields->size())
            return false;
        const auto& handle = (*v.fields)[unit->type_index].movement_class;
        v.movement = handle ? v.match->movement_map(*handle) : nullptr;
        return v.movement != nullptr;
    };
    sources.movement_class = [](void* user, int32_t x, int32_t z) -> uint8_t {
        const auto* map = static_cast<DebugGridView*>(user)->movement;
        if (x < 0 || z < 0 || static_cast<uint32_t>(x) >= map->width() ||
            static_cast<uint32_t>(z) >= map->height())
            return wr::movement_class_clear;
        return map->cell(static_cast<uint32_t>(x), static_cast<uint32_t>(z));
    };
    sources.search_cell = [](void* user, int32_t x, int32_t z) {
        const auto& search = static_cast<DebugGridView*>(user)->match->path_search_worker();
        if (x < 0 || z < 0 || static_cast<uint32_t>(x) >= search.width() ||
            static_cast<uint32_t>(z) >= search.height())
            return wr::DebugSearchCell{};
        const auto cell = search.cell(static_cast<uint32_t>(x), static_cast<uint32_t>(z));
        return wr::DebugSearchCell{cell.flags, cell.predecessor};
    };
    // The grid's random numbers are its own (DebugGridRandom): drawing it
    // never takes from the match's streams, and every draw of a tick shows
    // the numbers its first draw took.
    auto& kept = match_models().debug_random;
    start_debug_grid_draw(kept, match_->simulation().tick);
    sources.random.user = &kept;
    sources.random.next = [](void* user) {
        auto& grid = *static_cast<DebugGridRandom*>(user);
        return next_debug_grid_number(grid, [&] { return grid.generate(); });
    };
    oa::present::model::bridge_begin(bridge, frame, area, scale, display.palette);
    oa::present::model::bridge_open(
        bridge, {0, 0, bridge.surface.width - 1, bridge.surface.height - 1}
    );
    const auto raster = battlefield_raster();
    wr::overlay_debug_grid(game, raster, &bridge.surface, sources);
    if (!live_renderer_active())
        wr::overlay_debug_cursor_cross(game, raster, &bridge.surface);
    else if (game.debug_overlay == wr::debug_view::terrain) {
        const auto p = live_project_world(
            static_cast<int32_t>(game.cursor_position.x) / 65536.0F,
            static_cast<int32_t>(game.cursor_position.y) / 65536.0F,
            static_cast<int32_t>(game.cursor_position.z) / 65536.0F
        );
        const int32_t x = p.x - match_layout_.left + wr::battlefield_origin_x;
        const int32_t y = p.y - match_layout_.top + wr::battlefield_origin_y;
        constexpr int32_t arm = 8;
        const auto color = game.ui_colors[1];
        raster.line(raster.user, &bridge.surface, x - arm, y, x + arm, y, color);
        raster.line(raster.user, &bridge.surface, x, y - arm, x, y + arm, color);
    }
    oa::present::model::bridge_end(bridge);
}

void Runtime::begin_profile_window() {
    if (match_)
        profile::begin_window(match_->state().game.profile_times, clock_milliseconds());
}

void Runtime::mark_profile(int32_t category) {
    if (match_)
        profile::accumulate(
            match_->state().game.profile_times,
            clock_milliseconds(),
            static_cast<profile::Category>(category)
        );
}

void Runtime::draw_profile_bars() {
    if (!match_ || match_->state().game.profiling == 0)
        return;
    const oa::formats::fnt::Font* font = match_label_font();
    if (font == nullptr)
        return;
    ensure_ui_colors();
    const wr::OverlayRaster raster = source_overlay_raster();
    const auto font_height = static_cast<int32_t>(font->nominal_height & 0xff);
    for (int32_t category = 0; category < profile::category_count; ++category)
        wr::overlay_profile_bar(
            match_->state().game,
            raster,
            nullptr,
            oa::ui::display_layout::kSourceWidth,
            font_height,
            profile::category_labels[category],
            category
        );
}

void Runtime::draw_debug_status_line() {
    if (!match_)
        return;
    const oa::Game& game = match_->state().game;
    if ((game.outcome_flags & console::outcome_flag::debug_keys) == 0)
        return;
    // Its parts stand at the game font's columns.
    const PanelText panel(*this);
    const oa::formats::fnt::Font& font = message_font();
    ensure_ui_colors();
    // The counter counts the frames that draw the line, on the clock the
    // frame's match clock step read.
    const oa::ui::services::Clock clock{
        this,
        [](void* context) {
            return static_cast<const Runtime*>(context)->frame_clock_milliseconds();
        },
        nullptr
    };
    const int32_t rate = oa::ui::services::frame_rate_sample(&debug_line_rate_, &clock);
    const auto height = static_cast<int32_t>(static_cast<uint8_t>(font.nominal_height));
    const int32_t row = kDebugLineRows * height - kDebugLineRaise;
    const int scale = hud_text_scale();
    const uint8_t color = ui_colors_[kUiColorText];
    const auto part = [&](int32_t x, const char* text) {
        const auto at = hud_canvas(x, row);
        draw_match_text(&font, at.x, at.y, text, color, scale);
    };
    char text[48];
    std::snprintf(text, sizeof text, "FRATE: %d", static_cast<int>(rate));
    part(kDebugLineRateX, text);
    part(kDebugLineBuildX, kDebugLineBuild);
    const bool info = (game.outcome_flags & console::outcome_flag::debug_toggle_i) != 0;
    std::snprintf(
        text,
        sizeof text,
        "MODE %s INFO %s",
        kDebugLineMode,
        info ? kDebugLineInfoOn : kDebugLineInfoOff
    );
    part(kDebugLineModeX, text);
}

void Runtime::check_debug_status_line() {
    oa::Game& game = match_->state().game;
    const uint16_t kept = game.outcome_flags;
    // The echoed lines stay out of the compared frames.
    const auto capture = [&](uint16_t flags) {
        game.outcome_flags = flags;
        oa::sim::messages::clear_messages(game);
        render_match_surface();
        return surface_;
    };
    const auto keys = console::outcome_flag::debug_keys;
    const auto info = console::outcome_flag::debug_toggle_i;
    const auto without = capture(static_cast<uint16_t>(kept & ~keys));
    const auto info_off = capture(static_cast<uint16_t>((kept | keys) & ~info));
    const auto info_on = capture(static_cast<uint16_t>(kept | keys | info));
    game.outcome_flags = kept;
    // The line's row and its first and mode columns on the canvas.
    const auto& font = message_font();
    const auto height = static_cast<int32_t>(static_cast<uint8_t>(font.nominal_height));
    const int32_t row = kDebugLineRows * height - kDebugLineRaise;
    const auto start =
        oa::ui::display_layout::source_to_canvas(match_layout_, kDebugLineRateX, row);
    const auto mode = oa::ui::display_layout::source_to_canvas(match_layout_, kDebugLineModeX, row);
    const int line_bottom =
        start.y + static_cast<int>(oa::formats::fnt::line_height(font)) * hud_text_scale();

    // The box about the pixels two frames differ in within a region, and how
    // many they are.
    struct Changed {
        std::size_t pixels{};
        int left{std::numeric_limits<int>::max()};
        int top{std::numeric_limits<int>::max()};
        int right{std::numeric_limits<int>::min()};
        int bottom{std::numeric_limits<int>::min()};
    };

    const auto changed =
        [](const renderer::Surface& a, const renderer::Surface& b, int top, int bottom) {
            Changed box{};
            for (std::size_t at = 0; at + 2 < a.rgb.size() && at + 2 < b.rgb.size(); at += 3) {
                const auto x = static_cast<int>((at / 3) % a.width);
                const auto y = static_cast<int>((at / 3) / a.width);
                if (y < top || y > bottom ||
                    (a.rgb[at] == b.rgb[at] && a.rgb[at + 1] == b.rgb[at + 1] &&
                     a.rgb[at + 2] == b.rgb[at + 2]))
                    continue;
                ++box.pixels;
                box.left = std::min(box.left, x);
                box.right = std::max(box.right, x);
                box.top = std::min(box.top, y);
                box.bottom = std::max(box.bottom, y);
            }
            return box;
        };
    const auto text = [](const Changed& box) {
        return std::to_string(box.pixels) + " pixels in (" + std::to_string(box.left) + ", " +
               std::to_string(box.top) + ")-(" + std::to_string(box.right) + ", " +
               std::to_string(box.bottom) + ")";
    };
    const int any_row = std::numeric_limits<int>::max();
    // The line: text on its row, from its first column on. The debug keys
    // change other readouts too, elsewhere on the screen.
    const Changed line = changed(without, info_off, start.y - 1, line_bottom + 1);
    if (line.pixels < kTextMinPixels || line.left < start.x - 1)
        throw std::runtime_error(
            "console check: the debug keys' line changed " + text(line) + " on its row from (" +
            std::to_string(start.x) + ", " + std::to_string(start.y) + ") to y " +
            std::to_string(line_bottom)
        );
    // Debug key 'i': the line's mode words alone.
    const Changed words = changed(info_off, info_on, 0, any_row);
    if (words.pixels == 0 || words.left < mode.x - 1 || words.top < start.y - 1 ||
        words.bottom > line_bottom + 1)
        throw std::runtime_error(
            "console check: debug key 'i' changed " + text(words) +
            ", not the debug keys' line's mode words from x " + std::to_string(mode.x)
        );
}

void Runtime::run_console_crash_test(console::CrashTest test) {
    switch (test) {
    case console::CrashTest::exhaust_heap:
        oa::platform::exhaust_heap(nullptr);
        return;
    case console::CrashTest::exhaust_tagged_heap:
        oa::platform::exhaust_heap("heap exhaustion test");
        return;
    case console::CrashTest::divide:
        oa::platform::raise_divide_fault();
        return;
    case console::CrashTest::break_into_debugger:
        break;
    }
    if (sdl_.window != nullptr && (SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_FULLSCREEN) != 0) {
        // A window that stays full screen still breaks into the debugger;
        // only the desktop is harder to reach.
        std::ignore = SDL_SetWindowFullscreen(sdl_.window, false);
        SDL_Delay(kDebugBreakSettleMs);
    }
    release_pointer(sdl_.window);
    oa::platform::break_into_debugger();
}

void Runtime::keep_console_carry() {
    if (console_ && match_)
        console_->profiling = match_->state().game.profiling;
}

void Runtime::restore_console_carry() {
    if (console_ && match_)
        match_->state().game.profiling = console_->profiling;
}

namespace {

std::size_t pixel_offset(const renderer::Surface& frame, oa::ui::display_layout::Point at) {
    return (static_cast<std::size_t>(at.y) * frame.width + static_cast<std::size_t>(at.x)) * 3U;
}

bool same_rgb(const renderer::Surface& frame, std::size_t offset, const uint8_t* rgb) {
    return frame.rgb[offset] == rgb[0] && frame.rgb[offset + 1] == rgb[1] &&
           frame.rgb[offset + 2] == rgb[2];
}

} // namespace

void Runtime::check_console_debug_commands(const std::function<void(const char*)>& enter_line) {
    oa::Game& game = match_->state().game;
    // The echoed lines stay out of the compared frames.
    const auto capture = [&] {
        oa::sim::messages::clear_messages(game);
        render_match_surface();
        return surface_;
    };
    // The battlefield part of the frame; the top bar's readouts ease from
    // frame to frame.
    const auto low = oa::ui::display_layout::source_to_canvas(
        match_layout_, oa::ui::display_layout::kSourceLeft, oa::ui::display_layout::kSourceTop
    );
    const auto high = oa::ui::display_layout::source_to_canvas(
        match_layout_,
        oa::ui::display_layout::kSourceWidth,
        oa::ui::display_layout::kSourceBottomBarY
    );
    const auto each_battlefield_pixel = [&](const renderer::Surface& frame, const auto& visit) {
        for (int y = low.y; y < std::min(high.y, static_cast<int>(frame.height)); ++y)
            for (int x = low.x; x < std::min(high.x, static_cast<int>(frame.width)); ++x)
                visit(pixel_offset(frame, {x, y}));
    };
    const auto same_battlefield = [&](const renderer::Surface& a, const renderer::Surface& b) {
        bool same = true;
        each_battlefield_pixel(a, [&](std::size_t at) {
            same = same && same_rgb(a, at, &b.rgb[at]);
        });
        return same;
    };
    const auto before = capture();
    enter_line("+contour 3");
    if (console_->state.contour_values[0] != 3 * 256 || console_->state.contour_values[1] != 192)
        throw std::runtime_error("console check: +contour 3 did not set spacing 768 and phase 192");
    const auto contoured = capture();
    std::size_t ramp_pixels = 0, changed = 0;
    each_battlefield_pixel(contoured, [&](std::size_t at) {
        if (same_rgb(contoured, at, &before.rgb[at]))
            return;
        ++changed;
        for (const uint8_t index : wr::contour_ramp)
            if (same_rgb(contoured, at, &match_palette_[static_cast<std::size_t>(index) * 4U])) {
                ++ramp_pixels;
                break;
            }
    });
    if (ramp_pixels == 0 || ramp_pixels * 2 < changed)
        throw std::runtime_error(
            "console check: +contour drew " + std::to_string(ramp_pixels) + " ramp pixels of " +
            std::to_string(changed) + " changed"
        );
    enter_line("+contour 0");
    if (!same_battlefield(capture(), before))
        throw std::runtime_error("console check: +contour 0 left lines on the battlefield");

    const auto flags = console::console_flags(game);
    console::set_console_flags(
        game, static_cast<uint16_t>(flags & ~console::console_flag::developer)
    );
    enter_line("+profile");
    console::set_console_flags(game, flags);
    if (game.profiling != 0)
        throw std::runtime_error("console check: +profile ran without the passphrase");
    enter_line("+profile");
    if (game.profiling != 1)
        throw std::runtime_error("console check: +profile did not turn the bars on");
    constexpr std::array<int32_t, profile::category_count> kShown{10, 20, 5, 15, 0, 25, 5, 10, 10};
    static_assert(sizeof kShown == sizeof game.profile_times.shown);
    std::memcpy(game.profile_times.shown, kShown.data(), sizeof game.profile_times.shown);
    game.profile_times.shown_total = 100;
    const auto bars = capture();
    const oa::formats::fnt::Font* font = match_label_font();
    const int font_height = font != nullptr ? (font->nominal_height & 0xff) : 0;
    constexpr int kBarRight = oa::ui::display_layout::kSourceWidth - 0x5a;
    for (int32_t category = 0; category < profile::category_count; ++category) {
        if (kShown[static_cast<std::size_t>(category)] == 0)
            continue;
        const int y = font_height * category + 0x28 + font_height / 2;
        const int x = kBarRight - kShown[static_cast<std::size_t>(category)];
        const auto at =
            pixel_offset(bars, oa::ui::display_layout::source_to_canvas(match_layout_, x, y));
        if (!same_rgb(bars, at, &match_palette_[static_cast<std::size_t>(category + 1) * 4U]))
            throw std::runtime_error(
                "console check: +profile drew no bar in colour " + std::to_string(category + 1)
            );
    }
    const auto corner = pixel_offset(
        bars,
        oa::ui::display_layout::source_to_canvas(
            match_layout_, oa::ui::display_layout::kSourceWidth - 0x122, 0x26
        )
    );
    if (!same_rgb(bars, corner, &match_palette_[0xffU * 4U]))
        throw std::runtime_error("console check: +profile drew no frame in colour 0xFF");
    enter_line("+profile");
    if (game.profiling != 0 || !same_battlefield(capture(), before))
        throw std::runtime_error("console check: +profile did not take the bars away");
    // With the fixed clock a tick moves the clock by one tick's milliseconds,
    // charged at the first mark after the clock moved: the units.
    const bool fixed = options_.fixed_clock;
    options_.fixed_clock = true;
    begin_profile_window();
    ++match_timing_.tick;
    match_->simulation().tick = match_timing_.tick;
    tick_or_raise(*match_);
    begin_profile_window();
    options_.fixed_clock = fixed;
    if (game.profile_times.shown[OA_PROFILE_UNITS] != static_cast<int32_t>(kFixedClockMsPerTick) ||
        game.profile_times.shown_total != static_cast<int32_t>(kFixedClockMsPerTick))
        throw std::runtime_error("console check: a tick's time was not charged to the units");

    const auto real_crash = console_->host.crash_test;
    if (real_crash == nullptr)
        throw std::runtime_error("console check: DebugBreak has no crash hook");
    static std::vector<console::CrashTest> crashes;
    crashes.clear();
    console_->host.crash_test = [](void*, console::CrashTest test) { crashes.push_back(test); };
    const bool keys = (game.outcome_flags & console::outcome_flag::debug_keys) != 0;
    game.outcome_flags =
        static_cast<uint16_t>(game.outcome_flags & ~console::outcome_flag::debug_keys);
    enter_line("+debugbreak 3");
    const bool refused = crashes.empty();
    game.outcome_flags =
        static_cast<uint16_t>(game.outcome_flags | console::outcome_flag::debug_keys);
    for (const char* line :
         {"+debugbreak 1", "+debugbreak 2", "+debugbreak 3", "+debugbreak", "+debugbreak 4"})
        enter_line(line);
    if (!keys)
        game.outcome_flags =
            static_cast<uint16_t>(game.outcome_flags & ~console::outcome_flag::debug_keys);
    console_->host.crash_test = real_crash;
    const std::vector<console::CrashTest> expected{
        console::CrashTest::exhaust_heap,
        console::CrashTest::exhaust_tagged_heap,
        console::CrashTest::divide,
        console::CrashTest::break_into_debugger
    };
    if (!refused || crashes != expected)
        throw std::runtime_error(
            "console check: DebugBreak did not reach the crash tests as the game does"
        );
}

} // namespace oa::app
