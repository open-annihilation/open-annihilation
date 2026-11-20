// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The native check of the engine screens' scroll bars, driven through the SDL
// presenter as a player drives them, and the pointer helpers other checks use
// on scroll bars.
#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/formats/gaf.hpp"
#include "oa/sim/speed.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace input = oa::ui::gui_input;
using oa::ui::display_layout::Point;

// A horizontal knob's start frame sits this far past its position, and at
// least its width and this gap before the bar's last column.
constexpr int32_t kKnobInset = 3;
constexpr int32_t kKnobEndGap = 2;
// SOUNDS.GUI's FXVOL bound between the shared art's 9-pixel arrows, and
// SPEEDSRT.GUI's GAME in the preferences' PANEL filler.
constexpr input::ScrollRect kBoundVolume{285, 172, 103, 16};
constexpr int16_t kVolumePositions = 89;
constexpr int32_t kVolumeMaximum = 64;
constexpr input::ScrollRect kBoundGameSpeed{150, 151, 102, 16};
constexpr int16_t kGameSpeedPositions = 88;
// The GAME position that stands for game speed 20: 85 / 87 * 21, truncated.
constexpr int16_t kFastestGameSpeedPosition = 85;
// SELMAP.GUI's MAPNAMES holds 12 rows of 15 pixels once trimmed to 192
// pixels, and its SLIDER is 183 pixels long between its arrows.
constexpr int32_t kMapRows = 12;
constexpr int32_t kMapBarLength = 183;
constexpr int32_t kListKnobMargin = 3;
constexpr int32_t kMinimumListKnob = 10;
// NEWGAME.GUI's Missions list, 62 pixels high for any mission, holds 3 rows
// once trimmed to 48, and MissionsKnob is 54 pixels long between its arrows.
constexpr int32_t kMissionRows = 3;
constexpr int32_t kMissionBarLength = 54;
// NEWGAME.GUI's lists show rows of 15 pixels from 2 pixels below their top;
// a press 7 pixels into a row lands in its middle.
constexpr int32_t kListRowPitch = 15;
constexpr int32_t kListRowsTop = 2;
constexpr int32_t kListRowMiddle = 7;
// Ticks a held arrow waits before it repeats.
constexpr uint32_t kRepeatTicks = input::kScrollRepeatDelay;
// Source rows of the preferences' sub-panel that lie in the bottom bar's
// band, and columns sampled across it.
constexpr std::array<int32_t, 3> kSampledRows{449, 460, 475};
constexpr std::array<int32_t, 3> kSampledColumns{150, 200, 260};
// A point on SPEEDSRT.GUI's UNDO below the bottom bar's top row, and one
// below UNDO on the sub-panel's picture.
constexpr Point kOnUndo{200, 449};
constexpr Point kBelowUndo{200, 470};

/// Tells whether two scroll rectangles are the same.
bool same_rect(const input::ScrollRect& a, const input::ScrollRect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

/// Returns a picture's pixel.
const uint8_t* pixel_at(const renderer::Surface& picture, int32_t x, int32_t y) {
    return &picture.rgb
                [(static_cast<std::size_t>(y) * picture.width + static_cast<std::size_t>(x)) * 3U];
}

/// Tells whether a picture shows a bar's knob where 3.1c draws it.
///
/// Across a horizontal bar the knob is its start frame, 3 pixels past its
/// position and at least its width and 2 pixels before the bar's last column,
/// centred down the track's end frame; down a vertical bar its start frame
/// sits 3 pixels past its position, centred on the track.
bool knob_drawn(
    const renderer::Surface& picture,
    const input::ScrollBar& bar,
    const oa::formats::gaf::Sequence* art,
    const oa::PaletteBytes& palette,
    int32_t offset_x,
    int32_t offset_y
) {
    const auto* knob = oa::formats::gaf::frame_at(art, bar.art_base + input::kScrollKnobStart);
    const auto* end = oa::formats::gaf::frame_at(art, bar.art_base + input::kScrollTrackEnd);
    if (knob == nullptr || end == nullptr)
        return false;
    const auto rendered = oa::formats::gaf::render_normal(*knob);
    if (!rendered.ok())
        return false;
    int32_t x = 0;
    int32_t y = 0;
    if (bar.rect.width < bar.rect.height) {
        x = bar.rect.x + (end->width >> 1) - (knob->width >> 1);
        y = bar.knob + bar.rect.y + kKnobInset;
    } else {
        const int32_t right = bar.rect.x + bar.rect.width - 1;
        x = std::min(bar.knob + bar.rect.x + kKnobInset, right - knob->width - kKnobEndGap);
        y = bar.rect.y + (end->height >> 1) - (knob->height >> 1);
    }
    const auto& image = *rendered.frame;
    std::size_t covered = 0;
    for (uint32_t row = 0; row < image.height; ++row)
        for (uint32_t column = 0; column < image.width; ++column) {
            const auto at = static_cast<std::size_t>(row) * image.width + column;
            if (image.coverage[at] == 0)
                continue;
            const auto* shown = pixel_at(
                picture,
                offset_x + x + static_cast<int32_t>(column),
                offset_y + y + static_cast<int32_t>(row)
            );
            if (!std::equal(
                    shown,
                    shown + 3,
                    &palette[static_cast<std::size_t>(image.pixels[at]) * oa::palette_entry_bytes]
                ))
                return false;
            ++covered;
        }
    return covered != 0;
}

} // namespace

Point Runtime::scroll_canvas_point(int32_t x, int32_t y) const {
    if (screen_ == Screen::match) {
        const auto rows = preferences_panel_rows();
        if (rows.width > 0 && x >= rows.x && x < rows.x + rows.width && y >= rows.y &&
            y < rows.y + rows.height) {
            const auto scaled = [this](int32_t value) {
                return static_cast<int32_t>(
                    std::lround(static_cast<double>(value) * match_layout_.scale)
                );
            };
            return {scaled(x), scaled(y)};
        }
        return oa::ui::display_layout::source_to_canvas(match_layout_, x, y);
    }
    int32_t modal_x = 0;
    int32_t modal_y = 0;
    if (screen_ == Screen::map_selection && !resources_.layout.gadgets.empty()) {
        const auto& root = resources_.layout.gadgets.front().common;
        modal_x = (kCanvasWidth - root.width) / 2;
        modal_y = (kCanvasHeight - root.height) / 2;
    }
    const auto origin = panel_origin();
    return {x + modal_x + origin.x, y + modal_y + origin.y};
}

void Runtime::send_check_pointer(SDL_EventType type, Point canvas, uint8_t button, uint8_t clicks) {
    float x = static_cast<float>(canvas.x);
    float y = static_cast<float>(canvas.y);
    if (sdl_.renderer != nullptr && !frame_to_window(sdl_.renderer, x, y, &x, &y))
        throw std::runtime_error(std::string("SDL_RenderCoordinatesToWindow: ") + SDL_GetError());
    SDL_Event event{};
    event.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        event.motion.windowID = sdl_.window != nullptr ? SDL_GetWindowID(sdl_.window) : 0;
        event.motion.x = x;
        event.motion.y = y;
    } else {
        event.button.windowID = sdl_.window != nullptr ? SDL_GetWindowID(sdl_.window) : 0;
        event.button.button = button;
        event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.clicks = clicks;
        event.button.x = x;
        event.button.y = y;
    }
    bool running = true;
    dispatch_event(event, running);
    if (!running)
        throw std::runtime_error("a pointer event ended the run");
}

const renderer::LayoutScrolls::Bar& Runtime::check_scroll_bar(std::string_view name) {
    const bool over_match = screen_ == Screen::match;
    auto* scrolls = over_match ? hud_scrolls() : frontend_scrolls();
    const auto* layout =
        over_match ? (match_hud_ ? &match_hud_->layout : nullptr) : &resources_.layout;
    if (scrolls != nullptr && layout != nullptr)
        for (const auto& entry : scrolls->bars)
            if (entry.gadget < layout->gadgets.size() &&
                layout->gadgets[entry.gadget].common.name == name) {
                renderer::refresh_layout_scrolls(*scrolls, *layout);
                return entry;
            }
    throw std::runtime_error(
        "scroll bar check: " + std::string(name) + " is not a bound scroll bar"
    );
}

void Runtime::drag_check_knob(std::string_view name, int32_t pixels) {
    const auto bar = check_scroll_bar(name).bar;
    const auto knob = input::scroll_knob_rect(bar);
    const bool across = (bar.attributes & oa::ui::gui_layout::attribute::horizontal) != 0;
    const Point from{(knob.left + knob.right) / 2, (knob.top + knob.bottom) / 2};
    const Point to{from.x + (across ? pixels : 0), from.y + (across ? 0 : pixels)};
    send_check_pointer(SDL_EVENT_MOUSE_MOTION, scroll_canvas_point(from.x, from.y), 0);
    send_check_pointer(
        SDL_EVENT_MOUSE_BUTTON_DOWN, scroll_canvas_point(from.x, from.y), SDL_BUTTON_LEFT
    );
    send_check_pointer(SDL_EVENT_MOUSE_MOTION, scroll_canvas_point(to.x, to.y), 0);
    tick_scroll_bars();
    send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, scroll_canvas_point(to.x, to.y), SDL_BUTTON_LEFT);
}

void Runtime::click_check_arrow(std::string_view name, bool forward) {
    const auto bar = check_scroll_bar(name).bar;
    const auto& arrow = forward ? bar.forward_arrow : bar.back_arrow;
    const auto at = scroll_canvas_point(arrow.x + arrow.width / 2, arrow.y + arrow.height / 2);
    send_check_pointer(SDL_EVENT_MOUSE_MOTION, at, 0);
    send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, at, SDL_BUTTON_LEFT);
    send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, at, SDL_BUTTON_LEFT);
}

void Runtime::check_scroll_bars() {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("scroll bar check: " + what);
    };
    require(sdl_.renderer != nullptr && sdl_.window != nullptr, "needs the SDL presenter");
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    fake_frontend_tick_ = 1000U;
    const auto knob_of = [this](std::string_view name) { return check_scroll_bar(name).bar.knob; };
    const auto press = [this](Point at) {
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, scroll_canvas_point(at.x, at.y), 0);
        send_check_pointer(
            SDL_EVENT_MOUSE_BUTTON_DOWN, scroll_canvas_point(at.x, at.y), SDL_BUTTON_LEFT
        );
    };
    const auto release = [this](Point at) {
        send_check_pointer(
            SDL_EVENT_MOUSE_BUTTON_UP, scroll_canvas_point(at.x, at.y), SDL_BUTTON_LEFT
        );
    };
    const auto next_tick = [this] {
        *fake_frontend_tick_ += 1U;
        tick_scroll_bars();
    };
    const auto frontend_palette = [this]() -> const oa::PaletteBytes& {
        return resources_.background.palette ? *resources_.background.palette
                                             : resources_.gui_palette;
    };
    const auto shared_art = [](const oa::formats::gaf::Archive& shared) {
        return renderer::scroll_art_sequence(nullptr, shared, input::ScrollArt::shared);
    };

    // The options' SOUND panel: FXVOL between its arrows, its knob drawn,
    // and the volume set by each move of the knob.
    exercise_click(menu::resource_name(menu::Button::single_player));
    exercise_click(entry::resource_name(entry::Button::options));
    require(screen_ == Screen::options, "Options did not open STARTOPT.GUI");
    exercise_click("SOUND");
    require(screen_ == Screen::sound, "SOUND did not open its panel");
    for (int tries = 0; tries < 3 && check_scroll_bar("FXVOL").bar.grayed; ++tries)
        exercise_click("MODE");
    auto volume = check_scroll_bar("FXVOL").bar;
    require(!volume.grayed, "FXVOL stays grayed whatever the sound mode");
    require(
        same_rect(volume.rect, kBoundVolume) && volume.range == kVolumePositions &&
            volume.knob_size == 10 && volume.back_arrow.x == 276 && volume.back_arrow.width == 9 &&
            volume.forward_arrow.x == 388 && volume.art == input::ScrollArt::shared &&
            volume.art_base == input::kScrollHorizontalArt,
        "FXVOL is not bound between its arrows at 285,172, 103 pixels long with 89 positions"
    );
    const auto entry_volume = preferences_.fx_volume;
    // The value the knob stands for over the 0..64 the SOUND panel gives FXVOL.
    const auto volume_of = [&] {
        auto knob = check_scroll_bar("FXVOL").bar;
        knob.maximum = kVolumeMaximum;
        return input::scroll_value(knob);
    };
    require(
        static_cast<int32_t>(entry_volume) == volume_of(),
        "FXVOL's knob does not stand for the saved volume " + std::to_string(entry_volume)
    );
    auto frame = frame_without_cursor();
    require(
        knob_drawn(
            frame,
            check_scroll_bar("FXVOL").bar,
            shared_art(resources_.shared_sprites),
            frontend_palette(),
            0,
            0
        ),
        "FXVOL's knob is not drawn where it stands"
    );
    write_ppm(report_directory / "native-scroll-bars-sound.ppm", frame);
    // Bring the knob to the middle, then press beside it: one position.
    drag_check_knob("FXVOL", 40 - knob_of("FXVOL"));
    require(
        knob_of("FXVOL") == 40 && preferences_.fx_volume == static_cast<uint32_t>(volume_of()),
        "a drag did not move FXVOL's knob pixel for pixel to position 40 and set the volume"
    );
    const Point right_of_knob{kBoundVolume.x + kBoundVolume.width - 5, kBoundVolume.y + 8};
    press(right_of_knob);
    require(knob_of("FXVOL") == 40, "a press beside the knob moved it at once");
    release(right_of_knob);
    require(
        knob_of("FXVOL") == 41 && preferences_.fx_volume == static_cast<uint32_t>(volume_of()),
        "a click beside FXVOL's knob did not step it one position toward the pointer"
    );
    // A held press steps once a tick, and once more as it is released.
    const Point left_of_knob{kBoundVolume.x + 2, kBoundVolume.y + 8};
    press(left_of_knob);
    tick_scroll_bars();
    for (int step = 0; step < 5; ++step)
        next_tick();
    require(knob_of("FXVOL") == 36, "a held press did not step FXVOL's knob once a tick");
    release(left_of_knob);
    require(knob_of("FXVOL") == 35, "the release did not step FXVOL's knob once more");
    // An arrow steps at once, and repeats once a tick after 15 ticks.
    const auto forward = check_scroll_bar("FXVOL").bar.forward_arrow;
    const Point on_forward{forward.x + forward.width / 2, forward.y + forward.height / 2};
    press(on_forward);
    require(knob_of("FXVOL") == 36, "the forward arrow did not step the knob at once");
    tick_scroll_bars();
    for (uint32_t step = 1; step < kRepeatTicks; ++step)
        next_tick();
    require(knob_of("FXVOL") == 36, "a held arrow repeated before 15 ticks");
    next_tick();
    next_tick();
    require(knob_of("FXVOL") == 38, "a held arrow did not repeat once a tick after 15 ticks");
    release(on_forward);
    click_check_arrow("FXVOL", false);
    require(
        knob_of("FXVOL") == 37 && preferences_.fx_volume == static_cast<uint32_t>(volume_of()),
        "the back arrow did not step the knob back and set the volume"
    );
    // A layout that moves Page Up to F6 and End to F7 steps FXVOL by those
    // keys. Page Up and End themselves no longer do.
    {
        fs::path folder = user_folder();
        if (folder.empty())
            folder = "A04-page-keys";
        else
            folder /= "A04-page-keys";
        std::error_code make_error;
        fs::create_directories(folder, make_error);
        require(!make_error, "the Page Up layout folder could not be made");
        {
            std::ofstream out(folder / "oakeys.yaml", std::ios::binary);
            require(static_cast<bool>(out), "the Page Up layout could not be written");
            out << "oakeys: 1\n"
                << "id: a04-page-keys\n"
                << "name: \"Page keys\"\n"
                << "version: \"1.0\"\n"
                << "author: {name: \"Open Annihilation\"}\n"
                << "description: \"Page up is F6 and End is F7.\"\n"
                << "tags: [window]\n"
                << "base: oa-default\n"
                << "bindings:\n"
                << "  ui.screen-page-up: [\"F6\"]\n"
                << "  ui.screen-end: [\"F7\"]\n";
            require(static_cast<bool>(out), "the Page Up layout could not be written");
        }

        struct RestoreKeys {
            Runtime& runtime;
            std::string saved;

            ~RestoreKeys() {
                try {
                    runtime.options_.keys = saved;
                    runtime.key_bindings_refusal_.clear();
                    runtime.load_key_bindings();
                    runtime.ensure_key_bindings(false);
                } catch (...) {
                    // CANCEL and the later bars report a layout this one could not put back.
                }
            }
        } restore{*this, options_.keys};

        key_bindings_refusal_.clear();
        options_.keys = path_to_utf8(folder);
        load_key_bindings();
        ensure_key_bindings(false);
        require(
            key_bindings_refusal_.empty(),
            "the Page Up layout was refused: " + key_bindings_refusal_
        );
        int32_t fxvol = -1;
        for (std::size_t index = 1; index < resources_.layout.gadgets.size(); ++index) {
            if (resources_.layout.gadgets[index].common.name == "FXVOL") {
                fxvol = static_cast<int32_t>(index);
                break;
            }
        }
        require(fxvol > 0, "FXVOL is not on the sound panel");
        set_frontend_focus(fxvol);
        bool keys_running = true;
        const auto press_key = [&](SDL_Keycode key, SDL_Scancode scancode) {
            SDL_Event event{};
            event.type = SDL_EVENT_KEY_DOWN;
            event.key.key = key;
            event.key.scancode = scancode;
            event.key.down = true;
            dispatch_event(event, keys_running);
        };
        const int16_t page = input::scroll_page_span(check_scroll_bar("FXVOL").bar.range);
        require(page == 8, "FXVOL's page is not 8 positions");
        press_key(SDLK_F6, SDL_SCANCODE_F6);
        require(
            knob_of("FXVOL") == 37 - page, "F6 did not page FXVOL back after Page Up moved to F6"
        );
        press_key(SDLK_PAGEUP, SDL_SCANCODE_PAGEUP);
        require(knob_of("FXVOL") == 37 - page, "Page Up still stepped FXVOL after it moved to F6");
        press_key(SDLK_END, SDL_SCANCODE_END);
        require(knob_of("FXVOL") == 37 - page, "End still stepped FXVOL after it moved to F7");
        press_key(SDLK_F7, SDL_SCANCODE_F7);
        require(
            knob_of("FXVOL") == check_scroll_bar("FXVOL").bar.range - 1,
            "F7 did not take FXVOL to its end after End moved to F7"
        );
        require(frontend_focus() == fxvol, "the rebound page keys moved the focus off FXVOL");
    }
    exercise_click("CANCEL");
    require(
        screen_ == Screen::single_player && preferences_.fx_volume == entry_volume,
        "CANCEL did not put the effects volume back"
    );
    std::cout << "scroll bar check: SOUND's FXVOL steps, drags, holds and repeats as in 3.1c\n";

    // SELMAP.GUI: the map list's scroll bar shows when the maps overflow it,
    // with the knob sized from the rows it shows, and scrolls the list.
    exercise_click(entry::resource_name(entry::Button::skirmish));
    require(screen_ == Screen::skirmish, "Skirmish did not open SKIRMISH.GUI");
    exercise_click(skirmish::resource_name(skirmish::Button::select_map));
    require(
        screen_ == Screen::map_selection && !bound_map_names_.empty(),
        "Select Map did not open SELMAP.GUI with maps"
    );
    const auto maps = static_cast<int32_t>(bound_map_names_.size());
    const auto map_bar = check_scroll_bar("SLIDER").bar;
    require(
        map_bar.active == (maps > kMapRows) && map_bar.rect.height == kMapBarLength,
        "SELMAP.GUI's SLIDER does not show exactly when " + std::to_string(maps) +
            " maps overflow the list's 12 rows"
    );
    if (maps > kMapRows) {
        const auto knob_size = std::max(
            kMinimumListKnob,
            static_cast<int32_t>(
                static_cast<double>(kMapRows) / static_cast<double>(maps) *
                static_cast<double>(kMapBarLength - kListKnobMargin)
            )
        );
        require(
            map_bar.knob_size == knob_size &&
                map_bar.range == kMapBarLength - knob_size - kListKnobMargin,
            "SLIDER's knob is not 12/" + std::to_string(maps) + " of 180 pixels"
        );
        frame = frame_without_cursor();
        const auto& root = resources_.layout.gadgets.front().common;
        require(
            knob_drawn(
                frame,
                check_scroll_bar("SLIDER").bar,
                shared_art(resources_.shared_sprites),
                frontend_palette(),
                (kCanvasWidth - root.width) / 2,
                (kCanvasHeight - root.height) / 2
            ),
            "SLIDER's knob is not drawn where it stands"
        );
        write_ppm(report_directory / "native-scroll-bars-selmap.ppm", frame);
        // MAPNAMES holds the focus as SELMAP.GUI opens. Down moves the
        // selection a row and previews its map without choosing it, and
        // past the page's last row scrolls the list a row, the knob
        // following; Up moves back on the page. From Select Map, which Tab
        // focuses, Down moves the focus on instead.
        bool running = true;
        const auto press_key = [&](SDL_Keycode key, SDL_Scancode scancode, SDL_Keymod mod) {
            SDL_Event event{};
            event.type = SDL_EVENT_KEY_DOWN;
            event.key.key = key;
            event.key.scancode = scancode;
            event.key.mod = mod;
            event.key.down = true;
            dispatch_event(event, running);
        };
        const auto focused = [this] {
            const auto focus = frontend_focus();
            return focus < 0
                       ? std::string()
                       : resources_.layout.gadgets[static_cast<std::size_t>(focus)].common.name;
        };
        const auto summary = [this] {
            const auto* size = widget("SIZE");
            const auto* fields = size != nullptr
                                     ? std::get_if<oa::ui::gui_layout::LabelFields>(&size->fields)
                                     : nullptr;
            return fields != nullptr ? fields->text : std::string();
        };
        require(
            tdf_names_equal(focused(), "MAPNAMES"), "SELMAP.GUI did not open with MAPNAMES focused"
        );
        const auto chosen = skirmish_settings_.map_name;
        const auto start = static_cast<std::size_t>(std::max<int16_t>(0, modal_map_index_));
        const auto start_first = map_first_visible();
        const auto rows = static_cast<std::size_t>(kMapRows);
        require(
            start >= start_first && start < start_first + rows &&
                start_first + rows < static_cast<std::size_t>(maps),
            "SELMAP.GUI did not open on a page before the last with the saved map on it"
        );
        const auto to_edge = start_first + rows - start;
        for (std::size_t step = 1; step <= to_edge; ++step) {
            press_key(SDLK_DOWN, SDL_SCANCODE_DOWN, SDL_KMOD_NONE);
            const auto first = step < to_edge ? start_first : start_first + 1U;
            require(
                static_cast<std::size_t>(modal_map_index_) == start + step &&
                    map_first_visible() == first,
                "Down " + std::to_string(step) + " did not select map " +
                    std::to_string(start + step) + " with map " + std::to_string(first) +
                    " shown first"
            );
        }
        require(
            knob_of("SLIDER") == static_cast<int16_t>(
                                     (start_first + 1U) * static_cast<std::size_t>(map_bar.range) /
                                     static_cast<std::size_t>(maps - kMapRows)
                                 ),
            "the knob did not follow the list Down scrolled"
        );
        const auto keyed = summary();
        preview_map_index(static_cast<std::size_t>(modal_map_index_));
        require(
            summary() == keyed && skirmish_settings_.map_name == chosen,
            "Down did not preview the selected map without choosing it"
        );
        press_key(SDLK_UP, SDL_SCANCODE_UP, SDL_KMOD_NONE);
        require(
            static_cast<std::size_t>(modal_map_index_) == start + to_edge - 1U &&
                map_first_visible() == start_first + 1U,
            "Up did not move the selection back on the page"
        );
        const auto stepped = modal_map_index_;
        press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_NONE);
        press_key(SDLK_DOWN, SDL_SCANCODE_DOWN, SDL_KMOD_NONE);
        require(
            tdf_names_equal(focused(), "PREVMENU") && modal_map_index_ == stepped,
            "Down from Select Map did not move the focus to Cancel"
        );
        drag_check_knob("SLIDER", kMapBarLength);
        const auto last_page = static_cast<std::size_t>(maps - kMapRows);
        require(
            knob_of("SLIDER") == map_bar.range - 1 && map_first_visible() == last_page,
            "dragging SLIDER's knob to its end did not show the last page of maps"
        );
        // A click on the list picks the row it shows there.
        const auto* list = widget("MAPNAMES");
        require(list != nullptr, "SELMAP.GUI has no MAPNAMES");
        const Point first_row{list->common.x + 20, list->common.y + 2 + 7};
        press(first_row);
        release(first_row);
        require(
            modal_map_index_ == static_cast<int16_t>(last_page),
            "a click on the last page's first row did not pick map " + std::to_string(last_page)
        );
        click_check_arrow("SLIDER", false);
        const auto knob = knob_of("SLIDER");
        require(
            knob == map_bar.range - 2 &&
                map_first_visible() == static_cast<std::size_t>(
                                           static_cast<double>(last_page) * knob /
                                           static_cast<double>(map_bar.range - 1)
                                       ),
            "the up arrow did not scroll the list back with the knob"
        );
        write_ppm(report_directory / "native-scroll-bars-selmap-end.ppm", frame_without_cursor());
    }
    close_map_modal();
    std::cout << "scroll bar check: SELMAP.GUI's list of " << maps
              << " maps scrolls with its knob and steps with Up and Down\n";

    // NEWGAME.GUI for any mission, drawn once its setup has placed and filled
    // its lists: the Missions list shows 3 of the campaign's missions and
    // MissionsKnob, bound between its arrows, scrolls them.
    load(Screen::any_mission);
    require(screen_ == Screen::any_mission, "Any Mission did not open NEWGAME.GUI");
    const auto missions = static_cast<int32_t>(campaign_mission_labels_.size());
    const auto mission_bar = check_scroll_bar("MissionsKnob").bar;
    require(
        missions > kMissionRows && mission_bar.active &&
            mission_bar.rect.height == kMissionBarLength && mission_bar.rect.y == 398,
        "MissionsKnob is not shown bound between its arrows over " + std::to_string(missions) +
            " missions"
    );
    const auto mission_knob = std::max(
        kMinimumListKnob,
        static_cast<int32_t>(
            static_cast<double>(kMissionRows) / static_cast<double>(missions) *
            static_cast<double>(kMissionBarLength - kListKnobMargin)
        )
    );
    require(
        mission_bar.knob_size == mission_knob &&
            mission_bar.range == kMissionBarLength - mission_knob - kListKnobMargin,
        "MissionsKnob's knob is not 3/" + std::to_string(missions) + " of 51 pixels"
    );
    drag_check_knob("MissionsKnob", kMissionBarLength);
    rebuild_surface();
    require(
        frontend_list_first("Missions") == static_cast<std::size_t>(missions - kMissionRows) &&
            campaign_mission_first_visible_ == static_cast<std::size_t>(missions - kMissionRows),
        "dragging MissionsKnob to its end did not show the last missions"
    );
    const auto* missions_list = widget("Missions");
    require(missions_list != nullptr, "NEWGAME.GUI has no Missions list");
    const Point last_row{missions_list->common.x + 20, missions_list->common.y + 2 + 2 * 15 + 7};
    press(last_row);
    release(last_row);
    require(
        selected_mission_index_ == static_cast<std::size_t>(missions - 1),
        "a click on the list's last row did not pick the last mission"
    );
    write_ppm(report_directory / "native-scroll-bars-anymsn.ppm", frame_without_cursor());
    // The clicked list has the focus: Up selects the mission above, and a
    // double-click on the page's first row opens that mission's briefing, as
    // Start does.
    SDL_Event up{};
    up.type = SDL_EVENT_KEY_DOWN;
    up.key.key = SDLK_UP;
    up.key.scancode = SDL_SCANCODE_UP;
    up.key.down = true;
    bool running = true;
    dispatch_event(up, running);
    require(
        selected_mission_index_ == static_cast<std::size_t>(missions - 2) &&
            frontend_list_first("Missions") == static_cast<std::size_t>(missions - kMissionRows),
        "Up did not select the mission above the last one"
    );
    const auto first_row =
        scroll_canvas_point(missions_list->common.x + 20, missions_list->common.y + 2 + 7);
    send_check_pointer(SDL_EVENT_MOUSE_MOTION, first_row, 0);
    for (uint8_t click = 1; click <= 2; ++click) {
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, first_row, SDL_BUTTON_LEFT, click);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, first_row, SDL_BUTTON_LEFT, click);
    }
    require(
        screen_ == Screen::briefing && bound_mission_index() == missions - kMissionRows,
        "a double-click on a mission's row did not open its briefing"
    );
    std::cout << "scroll bar check: NEWGAME.GUI's list of " << missions
              << " missions scrolls with its knob, steps with Up and starts on a double-click\n";

    // A press on Any Mission's Campaign list gives it the focus from the
    // Missions list. A press on the selected campaign keeps the chosen
    // mission; Up and Down step the campaign and refill the Missions list
    // from its first row, even at the top of the list.
    load(Screen::any_mission);
    require(screen_ == Screen::any_mission, "Any Mission did not open NEWGAME.GUI again");
    const auto* campaign_list = widget("Campaign");
    const auto* shown_missions = widget("Missions");
    require(
        campaign_list != nullptr && shown_missions != nullptr,
        "NEWGAME.GUI has no Campaign or Missions list"
    );
    const auto campaign_top = frontend_list_first("Campaign");
    require(
        campaign_top.has_value() && *campaign_top + 1 < campaign_labels_.size(),
        "Any Mission's Campaign list is not bound over two campaigns or more"
    );
    const auto list_row = [](const oa::ui::gui_layout::Gadget& list, int32_t shown) {
        return Point{
            list.common.x + 20,
            list.common.y + kListRowsTop + shown * kListRowPitch + kListRowMiddle
        };
    };
    const auto click_row = [&](const oa::ui::gui_layout::Gadget& list, int32_t shown) {
        press(list_row(list, shown));
        release(list_row(list, shown));
    };
    const auto loaded_campaign = [&] {
        const char* name = oa::data::campaign::campaign_name_if_loaded(&campaign_object());
        return std::string(name != nullptr ? name : "");
    };
    click_row(*campaign_list, 0);
    require(
        selected_campaign_index_ == *campaign_top &&
            tdf_names_equal(loaded_campaign(), campaign_labels_[*campaign_top]) &&
            campaign_mission_labels_.size() > 1,
        "a click on the Campaign list's first row did not list that campaign's missions"
    );
    click_row(*shown_missions, 1);
    require(selected_mission_index_ == 1, "a click on the second mission did not pick it");
    click_row(*campaign_list, 0);
    require(
        selected_campaign_index_ == *campaign_top && selected_mission_index_ == 1,
        "a click on the selected campaign did not keep the chosen mission"
    );
    dispatch_event(up, running);
    const auto above = *campaign_top == 0 ? std::size_t{0} : *campaign_top - 1;
    require(
        selected_campaign_index_ == above && selected_mission_index_ == 0 &&
            frontend_list_first("Missions") == std::size_t{0},
        "Up on the Campaign list did not refill the Missions list from its first row"
    );
    SDL_Event down = up;
    down.key.key = SDLK_DOWN;
    down.key.scancode = SDL_SCANCODE_DOWN;
    dispatch_event(down, running);
    require(
        selected_campaign_index_ == above + 1 &&
            tdf_names_equal(loaded_campaign(), campaign_labels_[above + 1]),
        "Down on the Campaign list did not load the campaign below"
    );
    std::cout << "scroll bar check: Any Mission's Campaign list takes the focus and steps from '"
              << campaign_labels_[above] << "' to '" << campaign_labels_[above + 1] << "'\n";
    load(Screen::single_player);

    // The match's preferences in a window taller than the chrome and in a
    // 4:3 one: GAME's knob drawn and driven, and the sub-panel's last rows.
    for (const auto& [width, height] :
         {std::pair{kDefaultWindowWidth, kDefaultWindowHeight}, std::pair{1280, 960}}) {
        const auto size = std::to_string(width) + 'x' + std::to_string(height);
        if (match_)
            leave_match();
        load(Screen::main_menu);
        if (!SDL_SetWindowSize(sdl_.window, width, height) || !SDL_SyncWindow(sdl_.window))
            throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
        start_benchmark_skirmish();
        require(
            match_layout_.width == width && match_layout_.height == height,
            "the match canvas is not " + size
        );
        show_match_pause_menu();
        activate_pause_gadget("PREFS");
        activate_pause_gadget("SPEEDS");
        require(match_preferences_open(), "SPEEDS did not open the preferences' sub-panel");
        const auto game_speed = check_scroll_bar("GAME").bar;
        require(
            same_rect(game_speed.rect, kBoundGameSpeed) && game_speed.range == kGameSpeedPositions,
            "SPEEDSRT.GUI's GAME is not bound between its arrows at 150,151 with 88 positions"
        );
        render();
        require(
            knob_drawn(
                match_hud_cpu_,
                check_scroll_bar("GAME").bar,
                shared_art(match_hud_->sprites),
                renderer::grayed_paint(*match_hud_, hud_gray_table_).palette != nullptr
                    ? *renderer::grayed_paint(*match_hud_, hud_gray_table_).palette
                    : match_hud_->gui_palette,
                0,
                0
            ),
            "GAME's knob is not drawn where it stands on the " + size + " window"
        );
        drag_check_knob("GAME", kFastestGameSpeedPosition - knob_of("GAME"));
        auto& game = match_->state().game;
        require(
            knob_of("GAME") == kFastestGameSpeedPosition &&
                game.requested_speed == oa::sim::speed::fastest &&
                preferences_.game_speed == oa::sim::speed::fastest,
            "dragging GAME's knob did not set the running game's speed to " +
                std::to_string(oa::sim::speed::fastest)
        );

        // The sub-panel's last rows: over the battlefield below the chrome's
        // rows where the bottom bar sits apart, with the bar's own picture in
        // the bar, and in the bar where it joins the chrome.
        render();
        renderer::Surface composed;
        compose_match_layers(composed);
        write_ppm(report_directory / ("native-scroll-bars-preferences-" + size + ".ppm"), composed);
        const auto scale = match_layout_.scale;
        const auto scaled = [scale](int32_t value) {
            return static_cast<int32_t>(std::lround(static_cast<double>(value) * scale));
        };
        const bool joined =
            match_layout_.bottom_bar_y() == scaled(oa::ui::display_layout::kSourceBottomBarY);
        require(
            joined == (height * 4 == width * 3),
            "the bottom bar joins the chrome on a " + size + " window"
        );
        const auto& panel = joined ? match_hud_cpu_ : preferences_hud_;
        require(!panel.rgb.empty(), "no picture of the sub-panel on the " + size + " window");
        for (const auto row : kSampledRows)
            for (const auto column : kSampledColumns) {
                const auto* drawn = pixel_at(panel, column, row);
                const auto in_bar_y = match_layout_.bottom_bar_y() +
                                      scaled(row - oa::ui::display_layout::kSourceBottomBarY);
                if (joined) {
                    require(
                        std::equal(drawn, drawn + 3, pixel_at(composed, scaled(column), in_bar_y)),
                        "the sub-panel's row " + std::to_string(row) +
                            " does not show in the bottom bar on the " + size + " window"
                    );
                    continue;
                }
                require(
                    std::equal(drawn, drawn + 3, pixel_at(composed, scaled(column), scaled(row))),
                    "the sub-panel's row " + std::to_string(row) +
                        " does not show over the battlefield on the " + size + " window"
                );
                const auto& bar = match_hud_->background;
                const auto* own = &bar.rgb
                                       [(static_cast<std::size_t>(row) * bar.width +
                                         static_cast<std::size_t>(column)) *
                                        3U];
                require(
                    std::equal(own, own + 3, pixel_at(composed, scaled(column), in_bar_y)),
                    "the bottom bar shows the sub-panel's row " + std::to_string(row) + " on the " +
                        size + " window"
                );
            }
        const auto hovered_name = [this](Point source) {
            send_check_pointer(SDL_EVENT_MOUSE_MOTION, scroll_canvas_point(source.x, source.y), 0);
            return hovered_ && *hovered_ < match_hud_->layout.gadgets.size()
                       ? match_hud_->layout.gadgets[*hovered_].common.name
                       : std::string();
        };
        require(
            hovered_name(kOnUndo) == "UNDO" && hovered_name(kBelowUndo) != "UNDO",
            "the pointer over the sub-panel's last rows is not over its controls on the " + size +
                " window"
        );
        activate_pause_gadget("CANCEL");
        resume_match_pause();
        std::cout << "scroll bar check: the preferences' GAME knob and sub-panel on the " << size
                  << " window" << (joined ? " (bottom bar joined)" : " (bottom bar apart)") << '\n';
    }
    leave_match();
    fake_frontend_tick_.reset();
    if (!SDL_SetWindowSize(sdl_.window, kDefaultWindowWidth, kDefaultWindowHeight) ||
        !SDL_SyncWindow(sdl_.window))
        throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
}

} // namespace oa::app
