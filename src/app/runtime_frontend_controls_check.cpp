// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// MAINMENU.GUI's buttons under the pointer, held down and pressed by their
// quick keys, then the setup screens' options clicked through the SDL
// presenter as a player does: each click changes its setting and what the
// screen shows for it before Start, the help line follows the pointer, and the
// chosen options tab shows pressed. Alt+Enter switches the window to full
// screen and back on a menu and in a match. In a match's window Window edge
// Capture keeps the pointer in the window until a menu, Escape or a lost
// focus, Sticky puts it back on an edge until a push as firm as Edge
// stickiness lets it go, and Free leaves it free.
#include "engine_settings_state.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/languages/translation.hpp"
#include "oa/data/map_pack/map_name.hpp"
#include "oa/data/persist/save_sections.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/frontend_dialogs.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {

namespace {

// Captions of SKIRMISH.GUI's and NEWGAME.GUI's Difficulty stages.
constexpr std::array<std::string_view, 3> kDifficultyCaptions{"Easy", "Medium", "Hard"};
// The alliance image a row shows: frame 10 for an empty group, alliance*2+1
// for a lone member, alliance*2 for a shared group.
constexpr std::size_t kEmptyAllianceFrame = 10;
constexpr auto kLeft = static_cast<uint8_t>(SDL_BUTTON_LEFT);
constexpr auto kRight = static_cast<uint8_t>(SDL_BUTTON_RIGHT);
// A canvas point no gadget of the setup screens covers.
constexpr int32_t kParkX = 0;
constexpr int32_t kParkY = 0;
// The width of the message box Alt+Enter is pressed over, in canvas pixels.
constexpr int32_t kMessageBoxWidth = 300;
// A text list's rows start this many pixels below its top.
constexpr int32_t kListRowsTop = 2;

[[noreturn]] void fail(std::string_view what) {
    throw std::runtime_error("frontend controls check: " + std::string(what));
}

void require(bool condition, std::string_view what) {
    if (!condition)
        fail(what);
}

// <stem>-<step>.ppm beside --snapshot.
fs::path step_snapshot(const fs::path& snapshot, std::string_view step) {
    fs::path file = snapshot.parent_path() / snapshot.stem();
    file += '-' + std::string(step) + ".ppm";
    return file;
}

// Pixels that differ between two frames inside a canvas rectangle.
std::size_t changed_pixels(
    const renderer::Surface& before,
    const renderer::Surface& after,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
) {
    if (before.width != after.width || before.height != after.height)
        return static_cast<std::size_t>(std::max(width, 0)) *
               static_cast<std::size_t>(std::max(height, 0));
    std::size_t changed = 0;
    for (int32_t row = std::max(y, 0); row < std::min<int32_t>(y + height, before.height); ++row)
        for (int32_t column = std::max(x, 0); column < std::min<int32_t>(x + width, before.width);
             ++column) {
            const auto offset =
                (static_cast<std::size_t>(row) * before.width + static_cast<std::size_t>(column)) *
                3U;
            if (!std::equal(
                    before.rgb.begin() + static_cast<std::ptrdiff_t>(offset),
                    before.rgb.begin() + static_cast<std::ptrdiff_t>(offset + 3U),
                    after.rgb.begin() + static_cast<std::ptrdiff_t>(offset)
                ))
                ++changed;
        }
    return changed;
}

std::string_view controller_caption(int32_t controller) {
    switch (controller) {
    case entry::controller::disabled:
        return "Open";
    case entry::controller::human:
        return "Player";
    case entry::controller::computer:
        return "Computer";
    default:
        return {};
    }
}

} // namespace

void Runtime::check_frontend_controls() {
    namespace dialogs = oa::ui::frontend_dialogs;
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        fail("needs the SDL renderer");
    bool running = true;
    std::vector<std::string> problems;
    const auto expect = [&](bool condition, std::string what) {
        if (!condition) {
            std::cerr << "frontend controls check: " << what << '\n';
            problems.push_back(std::move(what));
        }
    };
    const auto snapshot = [&](std::string_view step) {
        if (!options_.snapshot.empty())
            write_ppm(step_snapshot(options_.snapshot, step), frame_without_cursor());
    };
    const auto send = [&](SDL_EventType type,
                          int32_t x,
                          int32_t y,
                          uint8_t button,
                          uint8_t clicks = 1) {
        float window_x = 0.0F;
        float window_y = 0.0F;
        require(
            frame_to_window(
                sdl_.renderer, static_cast<float>(x), static_cast<float>(y), &window_x, &window_y
            ),
            SDL_GetError()
        );
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
            event.button.clicks = clicks;
            event.button.x = window_x;
            event.button.y = window_y;
        }
        dispatch_event(event, running);
    };
    // The canvas point of a layout point: the map modal is centred.
    const auto canvas_origin = [&]() -> std::pair<int32_t, int32_t> {
        if (screen_ != Screen::map_selection || resources_.layout.gadgets.empty())
            return {0, 0};
        const auto& root = resources_.layout.gadgets.front().common;
        return {(kCanvasWidth - root.width) / 2, (kCanvasHeight - root.height) / 2};
    };
    const auto gadget_named = [&](std::string_view name) -> const oa::ui::gui_layout::Gadget& {
        const auto* gadget = widget(name);
        if (gadget == nullptr)
            fail(
                "screen " + std::to_string(static_cast<int>(screen_)) + " has no " +
                std::string(name)
            );
        return *gadget;
    };

    // The gadget's rectangle on the canvas.
    struct Box {
        int32_t x{};
        int32_t y{};
        int32_t width{};
        int32_t height{};
    };

    const auto box_of = [&](std::string_view name) {
        const auto& gadget = gadget_named(name);
        const auto [x, y] = canvas_origin();
        return Box{
            x + gadget.common.x, y + gadget.common.y, gadget.common.width, gadget.common.height
        };
    };
    const auto park_pointer = [&] {
        send(SDL_EVENT_MOUSE_MOTION, kParkX, kParkY, 0);
        require(!hovered_, "the parking point is over a gadget");
    };
    // The frame with nothing hovered, as the screen shows its settings.
    const auto frame = [&] {
        park_pointer();
        return frame_without_cursor();
    };
    // Clicks a gadget; the pointer stays over it.
    const auto click = [&](std::string_view name, uint8_t button = kLeft) {
        const auto box = box_of(name);
        const auto x = box.x + box.width / 2;
        const auto y = box.y + box.height / 2;
        send(SDL_EVENT_MOUSE_MOTION, x, y, 0);
        send(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, button);
        send(SDL_EVENT_MOUSE_BUTTON_UP, x, y, button);
        idle_tick();
    };
    // Moves the pointer onto a gadget without clicking.
    const auto hover = [&](std::string_view name) {
        const auto box = box_of(name);
        send(SDL_EVENT_MOUSE_MOTION, box.x + box.width / 2, box.y + box.height / 2, 0);
        require(
            hovered_ && resources_.layout.gadgets[*hovered_].common.name == name,
            "the pointer is not over " + std::string(name)
        );
    };
    // Whether a button's status is set, as the chosen member of its group.
    const auto pressed = [&](std::string_view name) {
        const auto* fields =
            std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget_named(name).fields);
        return fields != nullptr && fields->status != 0;
    };
    // The caption a button shows: its caption's segment for its stage.
    const auto caption = [&](std::string_view name) {
        const auto& gadget = gadget_named(name);
        const auto* fields = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
        if (fields == nullptr)
            fail(std::string(name) + " is not a button");
        const auto stage = widget_text_stages_.find(std::string(name));
        return std::string(
            renderer::staged_caption(
                fields->text, stage == widget_text_stages_.end() ? 0U : stage->second
            )
        );
    };
    const auto label_text = [&](std::string_view name) {
        const auto& gadget = gadget_named(name);
        if (const auto* fields = std::get_if<oa::ui::gui_layout::LabelFields>(&gadget.fields))
            return fields->text;
        if (const auto* fields = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields))
            return fields->text;
        fail(std::string(name) + " has no text");
    };
    const auto shown_frame = [&](std::string_view name) -> std::optional<std::size_t> {
        const auto found = widget_gaf_frames_.find(std::string(name));
        if (found == widget_gaf_frames_.end())
            return std::nullopt;
        return found->second;
    };
    const auto repainted = [&](const renderer::Surface& before,
                               const renderer::Surface& after,
                               std::string_view name) {
        const auto box = box_of(name);
        return changed_pixels(before, after, box.x, box.y, box.width, box.height);
    };
    // Clicks a staged button round its whole cycle: every click must show
    // another caption and repaint it, and the last one the first caption again.
    const auto cycle_captions = [&](std::string_view name) {
        const auto& gadget = gadget_named(name);
        const auto* fields = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
        if (fields == nullptr || fields->stages < 2)
            fail(std::string(name) + " is not a staged button");
        const auto stages = fields->stages;
        const auto first = caption(name);
        auto before = frame();
        for (int8_t step = 0; step < stages; ++step) {
            const auto shown = caption(name);
            click(name);
            const auto now = caption(name);
            auto after = frame();
            const auto pixels = repainted(before, after, name);
            std::cout << "frontend controls check: " << name << " '" << shown << "' -> '" << now
                      << "', " << pixels << " pixels repainted\n";
            expect(now != shown, std::string(name) + " still shows '" + shown + "' after a click");
            expect(pixels != 0, std::string(name) + " was not repainted after a click");
            before = std::move(after);
        }
        expect(
            caption(name) == first,
            std::string(name) + " did not come back to '" + first + "' after " +
                std::to_string(stages) + " clicks"
        );
    };

    // VISUALS' Screen Size: the display's sizes from the smallest to the
    // largest at the slider's ends (the made-up monitor's with
    // --display-modes), each shown as VIDVAL's "WIDTH x HEIGHT"; it opens on
    // the window's size, or on Custom where the display offers no such
    // size. With `pictures`, the largest is written as
    // <stem>-options-VISUALS-largest.ppm.
    const auto offered = EngineSettingsState::offered_screen_sizes(*this);
    const auto size_text = [](oa::ui::engine_settings::ScreenSize size) {
        return std::to_string(size.width) + " x " + std::to_string(size.height);
    };
    const char* const custom_shown = oa::data::languages::installed_translation(nullptr, "Custom");
    const std::string custom_text = custom_shown != nullptr ? custom_shown : "Custom";
    const auto check_screen_size_list = [&](bool pictures) {
        const auto now = EngineSettingsState::screen_size_now(*this);
        const auto opened = std::find(offered.begin(), offered.end(), now) != offered.end()
                                ? size_text(now)
                                : custom_text;
        expect(
            label_text("VIDVAL") == opened,
            "VISUALS' Screen Size opened on " + label_text("VIDVAL") + ", not on " + opened
        );
        // The slider is as long as its GUI makes it, whatever the count. The
        // knob goes to the first stop first: Custom rests it on the last
        // when the window is larger than every size listed.
        constexpr int32_t past_either_end = 400;
        drag_check_knob("VIDSLDR", -past_either_end);
        idle_tick();
        drag_check_knob("VIDSLDR", past_either_end);
        idle_tick();
        expect(
            label_text("VIDVAL") == size_text(offered.back()),
            "VIDSLDR's last stop shows " + label_text("VIDVAL") + ", not " +
                size_text(offered.back())
        );
        if (pictures) {
            park_pointer();
            snapshot("options-VISUALS-largest");
        }
        drag_check_knob("VIDSLDR", -past_either_end);
        idle_tick();
        expect(
            label_text("VIDVAL") == size_text(offered.front()),
            "VIDSLDR's first stop shows " + label_text("VIDVAL") + ", not " +
                size_text(offered.front())
        );
        std::cout << "frontend controls check: VISUALS offers " << offered.size()
                  << " screen sizes, " << size_text(offered.front()) << " to "
                  << size_text(offered.back()) << '\n';
    };

    // MAINMENU.GUI's buttons under the pointer: each is drawn as it is without
    // it. A press held over a button sinks it; it rises while the pointer is
    // off it and sinks again when the pointer comes back, and a release away
    // from it chooses nothing.
    require(screen_ == Screen::main_menu, "did not start on MAINMENU.GUI");
    // The frame as shown, the pointer drawn, for <stem>-<step>.ppm.
    const auto shown_snapshot = [&](std::string_view step) {
        if (options_.snapshot.empty())
            return;
        rebuild_surface();
        write_ppm(step_snapshot(options_.snapshot, step), surface_);
    };
    const auto idle_menu = frame();
    std::vector<std::string> menu_buttons;
    for (const auto& gadget : resources_.layout.gadgets) {
        const auto* fields = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
        if (gadget.common.type == oa::ui::gui_layout::GadgetType::button && fields != nullptr &&
            !fields->grayed_out)
            menu_buttons.push_back(gadget.common.name);
    }
    require(!menu_buttons.empty(), "MAINMENU.GUI has no buttons");
    for (const auto& name : menu_buttons) {
        hover(name);
        const auto pixels = repainted(idle_menu, frame_without_cursor(), name);
        std::cout << "frontend controls check: over " << name << ", " << pixels
                  << " pixels of it redrawn\n";
        expect(pixels == 0, name + " changes under the pointer");
        if (name == menu_buttons.front())
            shown_snapshot("menu-hover");
    }
    {
        const auto& name = menu_buttons.front();
        const auto box = box_of(name);
        const auto x = box.x + box.width / 2;
        const auto y = box.y + box.height / 2;
        send(SDL_EVENT_MOUSE_MOTION, x, y, 0);
        send(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, kLeft);
        const auto held = repainted(idle_menu, frame_without_cursor(), name);
        shown_snapshot("menu-held");
        send(SDL_EVENT_MOUSE_MOTION, kParkX, kParkY, 0);
        const auto held_off = repainted(idle_menu, frame_without_cursor(), name);
        shown_snapshot("menu-held-off");
        send(SDL_EVENT_MOUSE_MOTION, x, y, 0);
        const auto held_back = repainted(idle_menu, frame_without_cursor(), name);
        send(SDL_EVENT_MOUSE_MOTION, kParkX, kParkY, 0);
        send(SDL_EVENT_MOUSE_BUTTON_UP, kParkX, kParkY, kLeft);
        idle_tick();
        std::cout << "frontend controls check: " << name << " held " << held
                  << " pixels redrawn, off it " << held_off << ", back on it " << held_back << '\n';
        expect(held != 0, name + " held under the pointer is not drawn pressed");
        expect(held_off == 0, name + " stays pressed with the pointer off it");
        expect(held_back != 0, name + " is not drawn pressed when the pointer comes back");
        require(screen_ == Screen::main_menu, name + " released away from it was chosen");
        expect(repainted(idle_menu, frame(), name) == 0, name + " stays pressed after the release");
    }

    // Each MAINMENU.GUI button's quick key presses it, in either case, as a
    // click does, and a held key's repeats press nothing more: SINGLE opens
    // SINGLE.GUI, whose PrevMenu key comes back; MULTI opens the multiplayer
    // screens, which Escape leaves, or a notice saying why it cannot; INTRO,
    // with the display's full-screen flag cleared so that no movie plays,
    // says over the menu that it needs full screen; EXIT ends the run, and
    // the check goes on from the menu as it was.
    const auto type_key = [&](SDL_Keycode code, bool shifted = false, bool repeat = false) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
        event.key.key = code;
        event.key.mod = static_cast<SDL_Keymod>(shifted ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE);
        event.key.down = true;
        event.key.repeat = repeat;
        dispatch_event(event, running);
        event.type = SDL_EVENT_KEY_UP;
        event.key.down = false;
        event.key.repeat = false;
        dispatch_event(event, running);
        idle_tick();
    };
    // The key a button's quick key is typed with: a letter's key gives its
    // lower case, with Shift down for the upper case.
    const auto quick_key = [&](std::string_view name) {
        const auto* fields =
            std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget_named(name).fields);
        require(
            fields != nullptr && fields->quick_key != 0, std::string(name) + " has no quick key"
        );
        return static_cast<SDL_Keycode>(
            std::tolower(static_cast<unsigned char>(fields->quick_key))
        );
    };
    const auto on_main_menu = [&] {
        return screen_ == Screen::main_menu && state_.state == frontend::state_id::main_menu &&
               dialogs::dialog_count() == 0;
    };
    const auto single_key = quick_key(menu::resource_name(menu::Button::single_player));
    const auto multi_key = quick_key(menu::resource_name(menu::Button::multiplayer));
    const auto intro_key = quick_key(menu::resource_name(menu::Button::intro));
    const auto exit_key = quick_key(menu::resource_name(menu::Button::exit));
    for (const bool shifted : {false, true}) {
        type_key(single_key, shifted);
        require(screen_ == Screen::single_player, "SINGLE's quick key did not open SINGLE.GUI");
        // SINGLE's key is Skirmish's on SINGLE.GUI.
        type_key(single_key, shifted, true);
        require(screen_ == Screen::single_player, "SINGLE's held quick key pressed SINGLE.GUI's");
        type_key(quick_key(entry::resource_name(entry::Button::previous_menu)), !shifted);
        require(on_main_menu(), "PrevMenu's quick key did not return to the main menu");
    }
    type_key(multi_key);
    require(
        screen_ != Screen::main_menu || dialogs::dialog_count() != 0,
        "MULTI's quick key opened nothing"
    );
    if (dialogs::dialog_count() != 0) {
        auto context = screen_context();
        require(dialogs::dialog_click(&context, "OK"), "MULTI's notice has no OK");
        run_pending_notice_return();
    } else
        type_key(SDLK_ESCAPE);
    require(on_main_menu(), "Escape did not leave what MULTI's quick key opened");
    {
        const auto display_flags = state_.video_context_flags;
        state_.video_context_flags =
            static_cast<uint8_t>(display_flags & ~frontend::flags::fullscreen_mode);
        type_key(intro_key, true);
        state_.video_context_flags = display_flags;
        const bool told = dialogs::dialog_count() != 0;
        if (told) {
            auto context = screen_context();
            require(dialogs::dialog_click(&context, "OK"), "INTRO's message has no OK");
        }
        require(on_main_menu(), "INTRO's quick key left the main menu");
        require(told == offers_movies(), "INTRO's quick key did not press INTRO");
    }
    {
        const auto menu_state = state_;
        type_key(exit_key);
        const bool exited = exit_requested_;
        exit_requested_ = false;
        running = true;
        state_ = menu_state;
        require(exited, "EXIT's quick key did not end the run");
    }
    std::cout << "frontend controls check: MAINMENU.GUI's quick keys press SINGLE, MULTI, INTRO "
                 "and EXIT, and SINGLE.GUI's PrevMenu\n";

    // SKIRMISH.GUI.
    exercise_click(menu::resource_name(menu::Button::single_player));
    require(screen_ == Screen::single_player, "did not reach SINGLE.GUI");
    click(entry::resource_name(entry::Button::skirmish));
    require(screen_ == Screen::skirmish, "Skirmish did not open SKIRMISH.GUI");
    snapshot("skirmish-start");
    // SKIRMISH.GUI, and SELMAP.GUI over it, show in the main menu's palette,
    // so no pixel shows a colour only the screen's own picture's palette
    // holds: on SKIRMISH.GUI, the bright green of the entries its picture
    // does not use, where the focus marker's outline lights CommanderDeath.
    const auto expect_main_menu_palette = [&](std::string_view picture, std::string_view panel) {
        expect(
            main_menu_palette_ && screen_palette() == *main_menu_palette_,
            std::string(panel) + " does not show in the main menu's palette"
        );
        const auto& slots = named_backgrounds_.cache.slots;
        const auto* slot = std::find_if(std::begin(slots), std::end(slots), [&](const auto& kept) {
            return tdf_names_equal(kept.name, picture);
        });
        if (!main_menu_palette_ || slot == std::end(slots) || slot->palette == nullptr)
            return;
        const auto holds = [](const uint8_t* palette, const uint8_t* rgb) {
            for (std::size_t entry = 0; entry < oa::palette_color_count; ++entry)
                if (std::equal(rgb, rgb + 3, palette + entry * oa::palette_entry_bytes))
                    return true;
            return false;
        };
        const auto shown = frame();
        std::size_t foreign = 0;
        for (std::size_t pixel = 0; pixel * 3U < shown.rgb.size(); ++pixel) {
            const uint8_t* rgb = &shown.rgb[pixel * 3U];
            if (holds(slot->palette, rgb) && !holds(main_menu_palette_->data(), rgb))
                ++foreign;
        }
        expect(
            foreign == 0,
            std::string(panel) + " shows " + std::to_string(foreign) +
                " pixels in colours only its picture's own palette holds"
        );
    };
    expect_main_menu_palette("Skirmsetup4x", "SKIRMISH.GUI");
    // Every row shows the metal and energy of the setup's slot, each in the
    // range the row's clicks keep it in.
    const auto expect_row_resources = [&](const entry::SkirmishSettings& setup,
                                          std::string_view when) {
        for (int32_t row = 0; row < setup.slot_count; ++row) {
            const auto& row_slot = setup.slots[static_cast<std::size_t>(row)];
            for (const auto& [resource, amount] :
                 {std::pair{std::string_view("Metal"), row_slot.metal},
                  std::pair{std::string_view("Energy"), row_slot.energy}}) {
                const auto gadget = std::string(resource) + std::to_string(row);
                const auto shown = label_text(gadget);
                expect(
                    shown == std::to_string(amount) && amount >= skirmish::resource_minimum &&
                        amount <= skirmish::resource_maximum,
                    gadget + " shows '" + shown + "' " + std::string(when) + " for " +
                        std::to_string(amount)
                );
            }
        }
    };
    expect_row_resources(skirmish_settings_, "on opening");
    auto& rules = preferences_.skirmish;

    // The rule buttons, each with the SKIRMISH.GUI caption its setting shows.
    struct Rule {
        std::string_view name;
        std::string (*caption)(const init::Preferences&);
    };

    const std::array<Rule, 5> rule_buttons{{
        {"Difficulty",
         [](const init::Preferences& p) {
             return std::string(p.difficulty <= 2 ? kDifficultyCaptions[p.difficulty] : "");
         }},
        {"Mapping",
         [](const init::Preferences& p) {
             return std::string(p.skirmish.mapping == 0 ? "Mapped" : "Unmapped");
         }},
        {"CommanderDeath",
         [](const init::Preferences& p) {
             return std::string(p.skirmish.commander_death == 0 ? "Continues" : "Game ends");
         }},
        {"StartLocation",
         [](const init::Preferences& p) {
             return std::string(p.skirmish_location == 0 ? "Random" : "Fixed");
         }},
        {"LineOfSight", [](const init::Preferences& p) {
             return std::string(
                 p.skirmish.line_of_sight == 0 ? "Permanent"
                 : p.skirmish.los_type == 1    ? "True"
                                               : "Circular"
             );
         }},
    }};
    for (const auto& rule : rule_buttons)
        expect(
            caption(rule.name) == rule.caption(preferences_),
            std::string(rule.name) + " opens showing '" + caption(rule.name) +
                "' for a setting that reads '" + rule.caption(preferences_) + "'"
        );
    const auto settings_word = [&] {
        return std::to_string(preferences_.difficulty) + '/' +
               std::to_string(preferences_.skirmish_difficulty) + ' ' +
               std::to_string(rules.mapping) + ' ' + std::to_string(rules.commander_death) + ' ' +
               std::to_string(preferences_.skirmish_location) + ' ' +
               std::to_string(rules.line_of_sight) + '/' + std::to_string(rules.los_type);
    };
    // A map chosen on SELMAP.GUI, then one click of each rule button.
    auto before = frame();
    const auto map_before = skirmish_settings_.map_name;
    click(skirmish::resource_name(skirmish::Button::select_map));
    require(
        screen_ == Screen::map_selection && !bound_map_names_.empty(),
        "Select Map did not open SELMAP.GUI with maps"
    );
    require(bound_map_names_.size() > 1, "SELMAP.GUI lists a single map");
    expect_main_menu_palette("DSELECTMAP2", "SELMAP.GUI");
    // The list can name a map twice, one row after the other, so the map
    // chosen is the first row after the selected one that names another map.
    const auto current = static_cast<std::size_t>(std::max<int16_t>(0, modal_map_index_));
    auto chosen_index = (current + 1U) % bound_map_names_.size();
    while (chosen_index != current && bound_map_names_[chosen_index] == map_before)
        chosen_index = (chosen_index + 1U) % bound_map_names_.size();
    const auto chosen = bound_map_names_[chosen_index];
    require(chosen != map_before, "SELMAP.GUI lists no map but " + map_before);
    preview_map_index(chosen_index);
    // The map's summary names its players in the game's language, in the word
    // gamedata\translate.tdf gives for "Players": "Spieler" in German.
    const auto summary = label_text("SIZE");
    load_translations("German");
    preview_map_index(chosen_index);
    const auto german_summary = label_text("SIZE");
    load_translations(game_language());
    preview_map_index(chosen_index);
    std::cout << "frontend controls check: the map summary reads '" << summary << "', in German '"
              << german_summary << "'\n";
    expect(
        german_summary.find("  Spieler: ") != std::string::npos,
        "the map summary reads '" + german_summary + "' in German, without \"Spieler\""
    );
    expect(
        label_text("SIZE") == summary,
        "the map summary did not return to '" + summary + "' in the game's own language"
    );
    click(map_modal::resource_name(map_modal::Button::load));
    require(screen_ == Screen::skirmish, "Load did not return to SKIRMISH.GUI");
    auto after_map = frame();
    std::cout << "frontend controls check: map '" << map_before << "' -> '"
              << skirmish_settings_.map_name << "', MapName shows '" << label_text("MapName")
              << "', " << repainted(before, after_map, "MapName") << " pixels repainted\n";
    expect(skirmish_settings_.map_name == chosen, "Load did not choose " + chosen);
    // MapName shows a pack map, named name@package, by the words the skirmish
    // screen gives it, and a base map by its own name.
    const auto shown_map =
        oa::data::map_pack::split_pack_map_name(chosen) ? pack_map_shown_name(chosen) : chosen;
    expect(
        label_text("MapName") == shown_map,
        "MapName shows '" + label_text("MapName") + "' for the map " + chosen + " instead of '" +
            shown_map + "'"
    );
    expect(repainted(before, after_map, "MapName") != 0, "MapName was not repainted");
    for (const auto& rule : rule_buttons)
        expect(
            caption(rule.name) == rule.caption(preferences_),
            std::string(rule.name) + " shows '" + caption(rule.name) +
                "' after the map for a setting that reads '" + rule.caption(preferences_) + "'"
        );
    snapshot("skirmish-map");
    before = std::move(after_map);
    for (const auto& rule : rule_buttons) {
        const auto settings_before = settings_word();
        const auto help_before = gadget_named(rule.name).common.runtime_help;
        click(rule.name);
        const auto help = gadget_named(rule.name).common.runtime_help;
        const auto shown_help = label_text("HELPTEXT");
        if (rule.name == "Mapping")
            snapshot("skirmish-help");
        auto after = frame();
        const auto shown = caption(rule.name);
        const auto pixels = repainted(before, after, rule.name);
        std::cout << "frontend controls check: " << rule.name << " settings " << settings_before
                  << " -> " << settings_word() << ", shows '" << shown << "', help '" << shown_help
                  << "', " << pixels << " pixels repainted\n";
        expect(
            settings_word() != settings_before,
            std::string(rule.name) + " did not change its setting"
        );
        expect(
            shown == rule.caption(preferences_),
            std::string(rule.name) + " shows '" + shown + "' for a setting that reads '" +
                rule.caption(preferences_) + "'"
        );
        expect(pixels != 0, std::string(rule.name) + " was not repainted after a click");
        expect(
            preferences_.skirmish_difficulty == preferences_.difficulty,
            "Difficulty did not store the skirmish difficulty"
        );
        if (rule.name != "Difficulty") {
            expect(help != help_before, std::string(rule.name) + " kept its help after a click");
            expect(
                shown_help == help,
                "HELPTEXT shows '" + shown_help + "' over " + std::string(rule.name) +
                    " instead of its help '" + help + "'"
            );
        }
        before = std::move(after);
        if (rule.name == "Difficulty")
            snapshot("skirmish-difficulty");
    }
    snapshot("skirmish-rules");
    for (const auto& rule : rule_buttons)
        cycle_captions(rule.name);

    // The first rows' controls: each click changes the row's setting, and the
    // text or frame shown for it.
    before = frame();
    const auto slot_step = [&](std::string_view name,
                               uint8_t button,
                               auto&& setting,
                               auto&& shown_matches,
                               std::string_view what) {
        const auto value_before = setting();
        click(name, button);
        auto after = frame();
        const auto pixels = repainted(before, after, name);
        std::cout << "frontend controls check: " << name << ' ' << what << ' ' << value_before
                  << " -> " << setting() << ", " << pixels << " pixels repainted\n";
        expect(
            setting() != value_before, std::string(name) + " did not change " + std::string(what)
        );
        expect(shown_matches(), std::string(name) + " does not show " + std::string(what));
        expect(pixels != 0, std::string(name) + " was not repainted after a click");
        before = std::move(after);
    };
    auto& slots = skirmish_settings_.slots;
    for (int i = 0; i < 2; ++i)
        slot_step(
            "Player1",
            kLeft,
            [&] { return slots[1].controller; },
            [&] { return label_text("Player1") == controller_caption(slots[1].controller); },
            "its controller"
        );
    for (int i = 0; i < 2; ++i)
        slot_step(
            "Side0",
            kLeft,
            [&] { return slots[0].side; },
            [&] { return shown_frame("Side0") == static_cast<std::size_t>(slots[0].side); },
            "its side"
        );
    for (const auto button : {kLeft, kRight})
        slot_step(
            "Color0",
            button,
            [&] { return slots[0].color; },
            [&] { return shown_frame("Color0") == static_cast<std::size_t>(slots[0].color); },
            "its colour"
        );
    slot_step(
        "Allies0",
        kLeft,
        [&] { return slots[0].alliance; },
        [&] {
            const auto alliance = slots[0].alliance;
            const auto members = skirmish::alliance_members(skirmish_settings_, alliance);
            const auto expected = members == 0   ? kEmptyAllianceFrame
                                  : members == 1 ? static_cast<std::size_t>(alliance) * 2U + 1U
                                                 : static_cast<std::size_t>(alliance) * 2U;
            return shown_frame("Allies0") == expected;
        },
        "its allegiance"
    );
    snapshot("skirmish-rows");
    for (const std::string_view name : {"Metal0", "Energy0"}) {
        auto& amount = name == "Metal0" ? slots[0].metal : slots[0].energy;
        const auto start = amount;
        // Up then down, or down then up from the top.
        const bool up_first = amount < skirmish::resource_maximum;
        for (const bool up : {up_first, !up_first})
            slot_step(
                name,
                up ? kLeft : kRight,
                [&] { return amount; },
                [&] { return label_text(name) == std::to_string(amount); },
                "its amount"
            );
        expect(
            amount == start, std::string(name) + " did not come back to " + std::to_string(start)
        );
    }
    // The help line under the pointer: the allegiance and resource hints over
    // their row controls, nothing over the others. Each control without help
    // follows one with help, so its hover has to clear the line.
    constexpr std::array<std::pair<std::string_view, std::string_view>, 6> row_help{{
        {"Allies0", "Click to select an allegiance symbol."},
        {"Player0", ""},
        {"Metal0", "Left click to increase metal. Right click to decrease metal."},
        {"Side0", ""},
        {"Energy0", "Left click to increase energy. Right click to decrease energy."},
        {"Color0", ""},
    }};
    park_pointer();
    for (const auto& [name, help] : row_help) {
        hover(name);
        const auto shown_help = label_text("HELPTEXT");
        std::cout << "frontend controls check: over " << name << " HELPTEXT '" << shown_help
                  << "'\n";
        expect(
            shown_help == help,
            "HELPTEXT shows '" + shown_help + "' over " + std::string(name) + " instead of '" +
                std::string(help) + "'"
        );
        if (name == "Allies0")
            snapshot("skirmish-row-help");
    }

    // NEWGAME.GUI's difficulty and side.
    click(skirmish::resource_name(skirmish::Button::previous_menu));
    require(screen_ == Screen::single_player, "Previous Menu did not return to SINGLE.GUI");
    click(entry::resource_name(entry::Button::new_campaign));
    require(screen_ == Screen::new_campaign, "New Campaign did not open NEWGAME.GUI");
    expect(
        caption("Difficulty") == kDifficultyCaptions[preferences_.difficulty % 3U],
        "NEWGAME.GUI opens showing '" + caption("Difficulty") + "' for difficulty " +
            std::to_string(preferences_.difficulty)
    );
    cycle_captions("Difficulty");
    for (std::size_t step = 0; step < kDifficultyCaptions.size(); ++step) {
        click("Difficulty");
        expect(
            caption("Difficulty") == kDifficultyCaptions[preferences_.difficulty % 3U],
            "NEWGAME.GUI shows '" + caption("Difficulty") + "' for difficulty " +
                std::to_string(preferences_.difficulty)
        );
    }

    // The side buttons: the chosen side's Arm/Core button and emblem show
    // pressed, the other side's raised.
    struct SideChoice {
        std::string_view button;
        std::string_view emblem;
        uint32_t side;
    };

    constexpr std::array<SideChoice, 2> sides{{{"Arm", "Side0", 0}, {"Core", "Side1", 1}}};
    const auto shows_side = [&](uint32_t side) {
        const auto& chosen = sides[side];
        const auto& other = sides[1U - side];
        return pressed(chosen.button) && pressed(chosen.emblem) && !pressed(other.button) &&
               !pressed(other.emblem);
    };
    expect(
        preferences_.side <= 1U && shows_side(preferences_.side),
        "NEWGAME.GUI opens without showing side " + std::to_string(preferences_.side)
    );
    snapshot("newgame-start");
    before = frame();
    for (const auto side : {1U - (preferences_.side & 1U), preferences_.side & 1U}) {
        click(sides[side].button);
        auto after = frame();
        const auto pixels = repainted(before, after, sides[side].emblem) +
                            repainted(before, after, sides[1U - side].emblem);
        std::cout << "frontend controls check: " << sides[side].button << " side "
                  << preferences_.side << ", " << pixels << " emblem pixels repainted\n";
        expect(
            preferences_.side == side, std::string(sides[side].button) + " did not choose its side"
        );
        expect(
            shows_side(side),
            "NEWGAME.GUI does not show " + std::string(sides[side].button) + " chosen"
        );
        expect(
            repainted(before, after, sides[side].emblem) != 0 &&
                repainted(before, after, sides[1U - side].emblem) != 0,
            "the side emblems were not repainted after " + std::string(sides[side].button)
        );
        snapshot(side == 1U ? "newgame-core" : "newgame-arm");
        before = std::move(after);
    }

    // The Campaign list of the chosen side's campaigns: a click on another
    // campaign's row selects it and shows it selected without leaving the
    // screen, Down and Up move the selection a row, and a double-click on a
    // row opens that campaign's first briefing, as Start does.
    const auto campaign_rows = [&]() -> const oa::ui::gui_input::ScrollList& {
        auto* scrolls = frontend_scrolls();
        const auto index =
            static_cast<std::size_t>(&gadget_named("Campaign") - resources_.layout.gadgets.data());
        const auto* bound =
            scrolls != nullptr ? renderer::find_layout_list(*scrolls, index) : nullptr;
        if (bound == nullptr)
            fail("NEWGAME.GUI's Campaign list is not bound");
        return bound->list;
    };
    // The canvas point in the middle of a shown row of the Campaign list.
    const auto campaign_row_point = [&](std::size_t row) {
        const auto& rows = campaign_rows();
        const auto pitch =
            oa::ui::gui_input::scroll_list_pitch(rows, frontend_scrolls()->line_height);
        const auto box = box_of("Campaign");
        const auto shown = static_cast<int32_t>(row) - rows.first;
        return std::pair{box.x + box.width / 2, box.y + kListRowsTop + shown * pitch + pitch / 2};
    };
    const auto click_campaign_row = [&](std::size_t row, uint8_t clicks) {
        const auto [x, y] = campaign_row_point(row);
        send(SDL_EVENT_MOUSE_MOTION, x, y, 0);
        for (uint8_t click = 1; click <= clicks; ++click) {
            send(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, kLeft, click);
            send(SDL_EVENT_MOUSE_BUTTON_UP, x, y, kLeft, click);
        }
        idle_tick();
    };
    const auto press_key = [&](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.key = code;
        event.key.scancode = scancode;
        event.key.down = true;
        dispatch_event(event, running);
        idle_tick();
    };
    const auto campaigns = campaign_labels_.size();
    std::cout << "frontend controls check: NEWGAME.GUI lists " << campaigns << " campaigns:";
    for (const auto& label : campaign_labels_)
        std::cout << " '" << label << "'";
    std::cout << '\n';
    require(campaigns > 2, "NEWGAME.GUI does not list the installed campaigns");
    // The first shown row that names another campaign than the selected row.
    const auto page_rows = static_cast<std::size_t>(
        oa::ui::gui_input::scroll_list_page_rows(campaign_rows(), frontend_scrolls()->line_height)
    );
    const auto first_shown = static_cast<std::size_t>(std::max<int16_t>(0, campaign_rows().first));
    const auto shown_end = std::min(campaigns, first_shown + page_rows);
    auto picked = first_shown;
    while (picked < shown_end &&
           tdf_names_equal(campaign_labels_[picked], campaign_labels_[selected_campaign_index_]))
        ++picked;
    require(
        picked < shown_end && picked + 1 < campaigns,
        "the Campaign list shows no other campaign with a row below it"
    );
    const auto shows_campaign = [&](std::size_t row) {
        return selected_campaign_index_ == row &&
               campaign_rows().selection == static_cast<int16_t>(row);
    };
    const auto& picked_label = campaign_labels_[picked];
    before = frame();
    click_campaign_row(picked, 1);
    auto after = frame();
    const auto list_pixels = repainted(before, after, "Campaign");
    std::cout << "frontend controls check: a click on '" << picked_label << "' selects row "
              << selected_campaign_index_ << ", " << list_pixels << " list pixels repainted\n";
    expect(screen_ == Screen::new_campaign, "a click on '" + picked_label + "' left NEWGAME.GUI");
    expect(shows_campaign(picked), "a click on '" + picked_label + "' did not select it");
    expect(list_pixels != 0, "the Campaign list was not repainted after a click on another row");
    snapshot("newgame-campaign-picked");
    press_key(SDLK_DOWN, SDL_SCANCODE_DOWN);
    expect(shows_campaign(picked + 1), "Down did not select the campaign below the selected one");
    press_key(SDLK_UP, SDL_SCANCODE_UP);
    expect(shows_campaign(picked), "Up did not select the campaign above the selected one");
    // A pressed button takes the focus from the list: Down after Difficulty
    // leaves the chosen campaign alone. Three presses bring the difficulty
    // back round.
    for (std::size_t step = 0; step < kDifficultyCaptions.size(); ++step) {
        click("Difficulty");
        press_key(SDLK_DOWN, SDL_SCANCODE_DOWN);
        expect(shows_campaign(picked), "Down after Difficulty moved the Campaign list");
    }
    // CampaignKnob, shown when the campaigns overflow the list, scrolls it
    // with its forward arrow, a knob position at a time, and a click on the
    // row that brings into view selects that campaign.
    if (const auto knob = check_scroll_bar("CampaignKnob").bar; knob.active) {
        const auto first = campaign_rows().first;
        for (int32_t step = 0; step < knob.range && campaign_rows().first == first; ++step)
            click_check_arrow("CampaignKnob", true);
        idle_tick();
        const auto last_shown = std::min(
            campaigns - 1, static_cast<std::size_t>(campaign_rows().first) + page_rows - 1
        );
        std::cout << "frontend controls check: CampaignKnob's forward arrow shows row "
                  << campaign_rows().first << " first, '" << campaign_labels_[last_shown]
                  << "' last\n";
        expect(
            campaign_rows().first == first + 1,
            "CampaignKnob's forward arrow did not scroll the Campaign list"
        );
        click_campaign_row(last_shown, 1);
        expect(
            shows_campaign(last_shown),
            "a click on '" + campaign_labels_[last_shown] + "' scrolled into view did not select it"
        );
        snapshot("newgame-campaign-scrolled");
    }
    const auto& started_label = campaign_labels_[picked + 1];
    click_campaign_row(picked + 1, 2);
    const char* loaded = oa::data::campaign::campaign_name_if_loaded(&campaign_object());
    std::cout << "frontend controls check: a double-click on '" << started_label
              << "' shows screen " << static_cast<int>(screen_) << " for '"
              << (loaded != nullptr ? loaded : "") << "'\n";
    require(
        screen_ == Screen::briefing,
        "a double-click on '" + started_label + "' did not open its briefing"
    );
    expect(
        loaded != nullptr && tdf_names_equal(loaded, started_label),
        "a double-click on '" + started_label + "' did not start that campaign"
    );
    snapshot("newgame-campaign-briefing");
    click("PrevMenu");
    require(
        screen_ == Screen::new_campaign, "the briefing's PrevMenu did not go back to NEWGAME.GUI"
    );
    click("PrevMenu");
    require(screen_ == Screen::single_player, "Previous Menu did not leave NEWGAME.GUI");

    // The options tabs and the sub-panels' staged buttons; CANCEL puts the
    // options back. The sound and music panels keep their buttons' authored
    // foreground colour and draw them lit through the light table, so they are
    // exercised here too.
    click(entry::resource_name(entry::Button::options));
    require(screen_ == Screen::options, "Options did not open STARTOPT.GUI");
    constexpr std::array<std::string_view, 4> tabs{"SOUND", "SPEEDS", "VISUALS", "MUSIC"};
    const auto shows_tab = [&](std::string_view tab, std::string_view when) {
        for (const auto other : tabs)
            expect(
                pressed(other) == (other == tab),
                std::string(other) + (pressed(other) ? " shows pressed" : " shows raised") + ' ' +
                    std::string(when)
            );
    };
    shows_tab("", "before a tab is chosen");
    // The pixels a button's pressed status changes: the frame as shown
    // against the same frame drawn with the button raised.
    const auto pressed_pixels = [&](std::string_view name) {
        auto* gadget = widget(name);
        auto* fields = gadget != nullptr
                           ? std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget->fields)
                           : nullptr;
        if (fields == nullptr)
            fail(std::string(name) + " is not a button");
        const auto shown = frame();
        const auto status = fields->status;
        fields->status = 0;
        const auto raised = frame();
        fields->status = status;
        rebuild_surface();
        return repainted(raised, shown, name);
    };
    const std::array<std::pair<std::string_view, std::vector<std::string_view>>, 4> panels{{
        {"SOUND", {"SPEECH", "MODE"}},
        {"SPEEDS", {"LEFTCLICK", "UNITCHAT"}},
        {"VISUALS", {"SHADING", "ANTI", "BSHADOWS"}},
        {"MUSIC", {"NOTRAK", "TRACKTYPE", "TRACKMODE"}},
    }};
    // Whether the panel on screen has merged an art sequence by name.
    const auto merged_art = [&](std::string_view name) {
        return std::any_of(
            resources_.sprites.sequences.begin(),
            resources_.sprites.sequences.end(),
            [name](const oa::formats::gaf::Sequence& sequence) { return sequence.name == name; }
        );
    };
    for (const auto& [tab, buttons] : panels) {
        click(tab);
        // The music panel brings its own GAF, so its transport buttons draw
        // MUSIC.GAF's frames rather than the shared fallback frame.
        if (tab == "MUSIC") {
            expect(
                merged_art("CDPLAY") && merged_art("CDNEXT"),
                "MUSIC did not merge MUSIC.GAF's transport art"
            );
            // A transport button is hit where its frame is drawn: the record
            // takes the frame's size, and the pointer on the frame's right
            // edge, beyond the 16x16 the panel authors, is over the button.
            const auto sequence = std::find_if(
                resources_.sprites.sequences.begin(),
                resources_.sprites.sequences.end(),
                [](const oa::formats::gaf::Sequence& art) { return art.name == "CDPLAY"; }
            );
            const auto play = box_of("CDPLAY");
            int32_t drawn_width = play.width;
            if (sequence != resources_.sprites.sequences.end() && !sequence->frames.empty()) {
                const auto& art = sequence->frames.front();
                drawn_width = art.width;
                expect(
                    play.width == art.width && play.height == art.height,
                    "CDPLAY is hit as " + std::to_string(play.width) + 'x' +
                        std::to_string(play.height) + " but drawn " + std::to_string(art.width) +
                        'x' + std::to_string(art.height)
                );
            }
            send(SDL_EVENT_MOUSE_MOTION, play.x + drawn_width - 2, play.y + play.height / 2, 0);
            expect(
                hovered_ && resources_.layout.gadgets[*hovered_].common.name == "CDPLAY",
                "the right edge of CDPLAY's frame does not hit CDPLAY"
            );
            std::cout << "frontend controls check: CDPLAY is hit at " << play.x << ',' << play.y
                      << ' ' << play.width << 'x' << play.height << '\n';
        }
        const auto pixels = pressed_pixels(tab);
        std::cout << "frontend controls check: tab " << tab << " pressed "
                  << (pressed(tab) ? "yes" : "no") << ", " << pixels << " pixels drawn pressed\n";
        shows_tab(tab, "after " + std::string(tab) + " was clicked");
        expect(pixels != 0, std::string(tab) + " is not drawn pressed after it was clicked");
        park_pointer();
        snapshot("options-" + std::string(tab));
        if (tab == "VISUALS")
            check_screen_size_list(true);
        for (const auto button : buttons)
            cycle_captions(button);
        shows_tab(tab, "after the " + std::string(tab) + " panel's buttons were clicked");
    }
    click("CANCEL");
    require(screen_ == Screen::single_player, "CANCEL did not leave the options");
    const auto stored_size = [&] {
        const auto found =
            preference_values_.find(std::string(oa::ui::engine_settings::key::screen_size));
        return found == preference_values_.end() ? std::string("desktop") : found->second;
    };
    expect(stored_size() == "desktop", "CANCEL kept a Screen Size the player moved");
    // The window's own events (its new size among them) reach the game as
    // its loop pumps them.
    const auto settle_window = [&] {
        (void)SDL_SyncWindow(sdl_.window);
        SDL_Event event{};
        while (SDL_PollEvent(&event))
            dispatch_event(event, running);
    };
    const auto window_points = [&] {
        int width = 0;
        int height = 0;
        (void)SDL_GetWindowSize(sdl_.window, &width, &height);
        return std::to_string(width) + 'x' + std::to_string(height);
    };
    // The window's size as the check started, which it gets back at the end.
    int opened_width = 0;
    int opened_height = 0;
    (void)SDL_GetWindowSize(sdl_.window, &opened_width, &opened_height);
    // A window the player sized by hand to a size the display has no mode
    // of shows Custom in the language shown, and CANCEL keeps it as it is.
    constexpr oa::ui::engine_settings::ScreenSize kHandSized{1000, 700};
    require(
        std::find(offered.begin(), offered.end(), kHandSized) == offered.end(),
        "the display offers the size the window is dragged to"
    );
    require(SDL_SetWindowSize(sdl_.window, kHandSized.width, kHandSized.height), SDL_GetError());
    settle_window();
    click(entry::resource_name(entry::Button::options));
    click("VISUALS");
    expect(
        label_text("VIDVAL") == custom_text,
        "VISUALS shows " + label_text("VIDVAL") + " for a window dragged to " + window_points() +
            ", not " + custom_text
    );
    if (!options_.snapshot.empty()) {
        park_pointer();
        snapshot("options-VISUALS-custom");
    }
    click("CANCEL");
    require(screen_ == Screen::single_player, "CANCEL did not leave the options");
    expect(
        window_points() == "1000x700" && stored_size() == "desktop",
        "CANCEL on Custom changed the window to " + window_points() + " or the setting to " +
            stored_size()
    );
    // OK on a Screen Size moved to applies it at once: the window snaps to
    // the size, the menu follows it, the setting keeps it, and the options
    // open on it again. The knob goes to the first size, then steps on to
    // 1280x720.
    constexpr oa::ui::engine_settings::ScreenSize kChosenSize{1280, 720};
    const auto moved_to = std::find(offered.begin(), offered.end(), kChosenSize) != offered.end()
                              ? kChosenSize
                              : offered.back();
    const auto moved_to_text =
        std::to_string(moved_to.width) + 'x' + std::to_string(moved_to.height);
    click(entry::resource_name(entry::Button::options));
    click("VISUALS");
    drag_check_knob("VIDSLDR", -400);
    for (int step = 0; step < 400 && label_text("VIDVAL") != size_text(moved_to); ++step)
        click_check_arrow("VIDSLDR", true);
    idle_tick();
    expect(
        label_text("VIDVAL") == size_text(moved_to),
        "VIDSLDR's arrow never reached " + size_text(moved_to)
    );
    click("PREV");
    require(screen_ == Screen::single_player, "OK did not leave the options");
    settle_window();
    int menu_output_width = 0;
    int menu_output_height = 0;
    (void)SDL_GetRenderOutputSize(sdl_.renderer, &menu_output_width, &menu_output_height);
    expect(
        stored_size() == moved_to_text && engine_settings_state().current.screen_size == moved_to &&
            preferences_.display_width == moved_to.width &&
            preferences_.display_height == moved_to.height,
        "OK on Screen Size " + moved_to_text + " stored the setting as " + stored_size()
    );
    expect(
        window_points() == moved_to_text && menu_output_width == moved_to.width &&
            menu_output_height == moved_to.height,
        "OK on Screen Size " + moved_to_text + " left the window at " + window_points()
    );
    std::cout << "frontend controls check: VISUALS shows " << custom_text
              << " for a window dragged to 1000x700, and OK on " << moved_to_text
              << " snaps the window to it\n";
    // RESTORE shows the Screen size setting's default as the size it plays
    // at, Desktop's, never 3.1c's 640x480; UNDO brings the size the options
    // opened on back, and OK then keeps the setting. RESTORE and OK set it
    // to its default.
    const auto default_size =
        oa::ui::engine_settings::default_settings(EngineSettingsState::inputs(*this)).screen_size;
    const auto default_text = oa::ui::engine_settings::screen_size_text(default_size);
    const auto default_shown = EngineSettingsState::screen_size_shown(*this, default_size, offered);
    const auto kept_text = stored_size();
    const auto kept_shown = EngineSettingsState::screen_size_in_effect(*this, offered);
    click(entry::resource_name(entry::Button::options));
    click("VISUALS");
    expect(
        label_text("VIDVAL") == size_text(kept_shown),
        "VISUALS opened on " + label_text("VIDVAL") + " after OK on " + size_text(kept_shown)
    );
    click("RESTORE");
    idle_tick();
    expect(
        label_text("VIDVAL") == size_text(default_shown),
        "RESTORE showed Screen Size " + label_text("VIDVAL") + ", not the default's " +
            size_text(default_shown)
    );
    click("UNDO");
    idle_tick();
    expect(
        label_text("VIDVAL") == size_text(kept_shown),
        "UNDO after RESTORE showed Screen Size " + label_text("VIDVAL") + ", not " +
            size_text(kept_shown)
    );
    click("PREV");
    expect(
        stored_size() == kept_text,
        "OK after RESTORE and UNDO stored the setting as " + stored_size() + ", not " + kept_text
    );
    click(entry::resource_name(entry::Button::options));
    click("VISUALS");
    click("RESTORE");
    idle_tick();
    click("PREV");
    expect(
        stored_size() == default_text &&
            engine_settings_state().current.screen_size == default_size,
        "OK after RESTORE stored the setting as " + stored_size() + ", not " + default_text
    );
    std::cout << "frontend controls check: RESTORE shows Screen Size " << size_text(default_shown)
              << " and OK stores " << default_text << '\n';
    // Desktop in a window leaves it as it is.
    expect(
        window_points() == moved_to_text,
        "OK after RESTORE changed the window to " + window_points()
    );
    // The check's preferences file starts the next run at Desktop again, and
    // the window goes back to its size.
    EngineSettingsState::choose_screen_size(*this, oa::ui::engine_settings::desktop_screen_size);
    save_preferences();
    expect(stored_size() == "desktop", "the Screen size setting did not go back to Desktop");
    require(SDL_SetWindowSize(sdl_.window, opened_width, opened_height), SDL_GetError());
    settle_window();

    // Alt+Enter switches the window to full screen and back, on a menu and
    // in a match: Return and keypad Enter alike, a held key's repeats
    // switching nothing and reaching no screen, the screen following the
    // window's new size, and full screen keeping the pointer on the window.
    const auto key = [&](SDL_EventType type, SDL_Keycode code, SDL_Keymod mod, bool repeat) {
        SDL_Event event{};
        event.type = type;
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.key = code;
        event.key.scancode =
            code == SDLK_KP_ENTER ? SDL_SCANCODE_KP_ENTER : SDL_GetScancodeFromKey(code, nullptr);
        event.key.mod = mod;
        event.key.repeat = repeat;
        event.key.down = type == SDL_EVENT_KEY_DOWN;
        dispatch_event(event, running);
    };
    // The window's own events (its new size, full screen entered or left)
    // reach the game as its loop pumps them.
    const auto settle = [&] {
        (void)SDL_SyncWindow(sdl_.window);
        SDL_Event event{};
        while (SDL_PollEvent(&event))
            dispatch_event(event, running);
    };
    // Alt+Enter pressed, held for some repeats and released.
    const auto alt_enter = [&](SDL_Keycode code, int repeats) {
        key(SDL_EVENT_KEY_DOWN, code, SDL_KMOD_LALT, false);
        for (int repeat = 0; repeat < repeats; ++repeat)
            key(SDL_EVENT_KEY_DOWN, code, SDL_KMOD_LALT, true);
        key(SDL_EVENT_KEY_UP, code, SDL_KMOD_LALT, false);
        settle();
    };
    const auto full_screen = [&] {
        return (SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_FULLSCREEN) != 0;
    };
    const auto window_size = [&] {
        int width = 0;
        int height = 0;
        (void)SDL_GetWindowSizeInPixels(sdl_.window, &width, &height);
        return std::to_string(width) + 'x' + std::to_string(height);
    };
    // Whether the window lies on its display's usable area, as a window that
    // leaves full screen is brought back (bring_window_on_display).
    const auto on_display = [&] {
        SDL_Rect window{};
        SDL_Rect usable{};
        return SDL_GetWindowPosition(sdl_.window, &window.x, &window.y) &&
               SDL_GetWindowSize(sdl_.window, &window.w, &window.h) &&
               SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(sdl_.window), &usable) &&
               window.x >= usable.x && window.y >= usable.y &&
               window.x + window.w <= usable.x + usable.w &&
               window.y + window.h <= usable.y + usable.h;
    };
    // Full screen with the focus keeps the pointer on the window. A window
    // off the battlefield leaves it free. A match's window may hold it.
    bool pointer_kept_in_full_screen = false;
    const auto expect_pointer_bounds = [&](std::string_view where) {
        const bool focused = (SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        const bool kept = SDL_GetWindowMouseGrab(sdl_.window);
        if (full_screen() && focused)
            expect(
                kept, "full screen does not keep the pointer on the window " + std::string(where)
            );
        else if (!focused)
            expect(!kept, "the pointer is held without the focus " + std::string(where));
        else if (screen_ != Screen::match || !match_ || match_paused_ || match_finished_)
            expect(!kept, "the pointer is held off the battlefield " + std::string(where));
        pointer_kept_in_full_screen = pointer_kept_in_full_screen || (kept && full_screen());
    };
    // The screen is laid out at the window's size: a match's frame takes
    // the window's pixels, and a menu is drawn to the whole window.
    const auto follows_window = [&] {
        int width = 0;
        int height = 0;
        int output_width = 0;
        int output_height = 0;
        (void)SDL_GetWindowSizeInPixels(sdl_.window, &width, &height);
        (void)SDL_GetRenderOutputSize(sdl_.renderer, &output_width, &output_height);
        if (output_width != width || output_height != height)
            return false;
        return screen_ != Screen::match ||
               (match_layout_.width == width && match_layout_.height == height &&
                output_texture_w_ == width && output_texture_h_ == height);
    };
    // The window's picture as it is presented, at the window's size.
    const auto snapshot_window = [&](std::string_view step) {
        if (options_.snapshot.empty())
            return;
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        write_ppm(step_snapshot(options_.snapshot, step), presented);
    };
    const auto switches_twice = [&](std::string_view where, Screen screen) {
        const bool started_full_screen = full_screen();
        const auto started_size = window_size();
        const bool started_on_display = started_full_screen || on_display();
        alt_enter(SDLK_RETURN, 3);
        const auto switched_size = window_size();
        expect(
            full_screen() != started_full_screen,
            "Alt+Enter did not switch the window's mode " + std::string(where)
        );
        expect(
            follows_window(),
            "the screen does not follow the window's size " + switched_size + ' ' +
                std::string(where)
        );
        expect_pointer_bounds(where);
        const std::string step = where == "on a menu" ? "alt-enter-menu" : "alt-enter-match";
        snapshot_window(step + "-switched");
        alt_enter(SDLK_KP_ENTER, 3);
        expect(
            full_screen() == started_full_screen,
            "Alt+keypad Enter did not switch the window back " + std::string(where)
        );
        // A window comes back at its own size, and one that was off its
        // display comes back onto it, shrunk when it was too large for it.
        if (started_on_display)
            expect(
                window_size() == started_size,
                "the window came back at " + window_size() + ", not " + started_size + ' ' +
                    std::string(where)
            );
        else
            expect(
                on_display(),
                "the window came back off its display at " + window_size() + ' ' +
                    std::string(where)
            );
        expect(
            follows_window(),
            "the screen does not follow the window's size " + window_size() + " back " +
                std::string(where)
        );
        expect(
            screen_ == screen, "Alt+Enter left the screen it was pressed on " + std::string(where)
        );
        expect_pointer_bounds(std::string(where) + " back");
        snapshot_window(step + "-back");
        std::cout << "frontend controls check: Alt+Enter " << where << " switched to "
                  << (started_full_screen ? "a window" : "full screen") << " (" << started_size
                  << " to " << switched_size << ") and back at " << window_size() << '\n';
    };
    switches_twice("on a menu", Screen::single_player);
    // Over a message box, a held Alt+Enter switches the window and leaves the
    // box open: its repeats never press OK, the box's Enter default.
    show_frontend_message("Alt+Enter", kMessageBoxWidth, 1, 0);
    require(dialogs::dialog_count() == 1, "the message box did not open");
    const bool before_box_switch = full_screen();
    alt_enter(SDLK_RETURN, 3);
    expect(full_screen() != before_box_switch, "Alt+Enter did not switch over a message box");
    expect(dialogs::dialog_count() == 1, "a held Alt+Enter pressed the message box's OK");
    snapshot_window("alt-enter-box");
    // Alt let go of before Enter: Enter's repeats and its release still
    // belong to the switch, and press no OK either.
    key(SDL_EVENT_KEY_DOWN, SDLK_RETURN, SDL_KMOD_LALT, false);
    for (int repeat = 0; repeat < 3; ++repeat)
        key(SDL_EVENT_KEY_DOWN, SDLK_RETURN, SDL_KMOD_NONE, true);
    key(SDL_EVENT_KEY_UP, SDLK_RETURN, SDL_KMOD_NONE, false);
    settle();
    expect(
        full_screen() == before_box_switch,
        "a second Alt+Enter over a message box did not switch back"
    );
    expect(
        dialogs::dialog_count() == 1,
        "Enter held on after Alt was let go pressed the message box's OK"
    );
    key(SDL_EVENT_KEY_DOWN, SDLK_RETURN, SDL_KMOD_NONE, false);
    key(SDL_EVENT_KEY_UP, SDLK_RETURN, SDL_KMOD_NONE, false);
    expect(dialogs::dialog_count() == 0, "Enter did not close the message box");
    require(screen_ == Screen::single_player, "the message box left SINGLE.GUI");

    click(entry::resource_name(entry::Button::skirmish));
    require(screen_ == Screen::skirmish, "Skirmish did not open SKIRMISH.GUI");
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    require(screen_ == Screen::match && match_, "Start did not enter a match");
    apply_output_mode();
    switches_twice("in a match", Screen::match);
    expect(!chat_composing_, "a held Alt+Enter opened the chat line in a match");
    // With the chat line open, Alt+Enter switches the window and leaves the
    // line as it was.
    key(SDL_EVENT_KEY_DOWN, SDLK_RETURN, SDL_KMOD_NONE, false);
    key(SDL_EVENT_KEY_UP, SDLK_RETURN, SDL_KMOD_NONE, false);
    require(chat_composing_, "Enter did not open the chat line");
    const bool before_chat_switch = full_screen();
    alt_enter(SDLK_RETURN, 3);
    expect(full_screen() != before_chat_switch, "Alt+Enter did not switch over the chat line");
    expect(
        chat_composing_ && chat_buffer_.empty(), "Alt+Enter submitted or changed the open chat line"
    );
    alt_enter(SDLK_RETURN, 0);
    expect(
        full_screen() == before_chat_switch,
        "a second Alt+Enter over the chat line did not switch back"
    );
    key(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE, SDL_KMOD_NONE, false);
    expect(!chat_composing_, "Escape did not close the chat line");

    // Window edge in a match's window. Capture, the default, keeps the
    // pointer in the window through any push or flick, and lets it go in
    // the game menu, on the pause screen, in Settings, after Escape and
    // without the focus; a click on the battlefield captures it again, and
    // the click that only brings the focus back reaches nothing. Sticky puts
    // the pointer back on an edge until a push as firm as Edge stickiness
    // lets it go, and 0.0 never holds. Free leaves it free. This driver is
    // not Wayland, so Sticky stays free of a grab.
    {
        namespace edge_settings = oa::ui::engine_settings;
        const bool sticky_started_full = full_screen();
        if (sticky_started_full)
            alt_enter(SDLK_RETURN, 0);
        expect(
            (SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_MOUSE_FOCUS) != 0,
            "the window does not have the pointer for Window edge"
        );
        int sticky_width = 0;
        int sticky_height = 0;
        require(
            SDL_GetWindowSize(sdl_.window, &sticky_width, &sticky_height) == true, SDL_GetError()
        );
        require(sticky_width > 80 && sticky_height > 80, "the window is too small for Window edge");
        const auto saved_edge = engine_settings_state().current.window_edge;
        const auto saved_level = engine_settings_state().current.edge_stickiness;
        const float sticky_right = static_cast<float>(sticky_width - 1);
        const float sticky_bottom = static_cast<float>(sticky_height - 1);
        const float sticky_mid_x = static_cast<float>(sticky_width / 2);
        const float sticky_mid_y = static_cast<float>(sticky_height / 2);
        uint64_t sticky_time_ms = SDL_GetTicks();
        const auto sticky_move =
            [&](float x, float y, float motion_x, float motion_y, uint64_t step_ms) {
                sticky_time_ms += step_ms;
                SDL_Event moved{};
                moved.type = SDL_EVENT_MOUSE_MOTION;
                moved.motion.windowID = SDL_GetWindowID(sdl_.window);
                moved.motion.x = x;
                moved.motion.y = y;
                moved.motion.xrel = motion_x;
                moved.motion.yrel = motion_y;
                moved.motion.timestamp = sticky_time_ms * 1'000'000ULL;
                dispatch_event(moved, running);
            };
        const auto press = [&](SDL_EventType type, float x, float y) {
            SDL_Event pressed{};
            pressed.type = type;
            pressed.button.windowID = SDL_GetWindowID(sdl_.window);
            pressed.button.button = SDL_BUTTON_LEFT;
            pressed.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            pressed.button.clicks = 1;
            pressed.button.x = x;
            pressed.button.y = y;
            dispatch_event(pressed, running);
        };
        const auto window_event = [&](SDL_EventType type) {
            SDL_Event changed{};
            changed.type = type;
            changed.window.windowID = SDL_GetWindowID(sdl_.window);
            dispatch_event(changed, running);
        };
        const auto saved_camera_x = match_camera_x_;
        const auto saved_camera_z = match_camera_z_;
        const auto saved_clock = scroll_clock_;
        const auto saved_frame = frame_time_ns_;
        // One second of edge scrolling from where the pointer rests.
        const auto pan_once = [&] {
            scroll_clock_ = frame_pacing::kNanosecondsPerSecond;
            frame_time_ns_ = frame_pacing::kNanosecondsPerSecond * 2;
            pan_match_camera();
        };

        // Capture: the window's grab, whatever the pointer does.
        engine_settings_state().current.window_edge = edge_settings::WindowEdge::capture;
        const auto captured = [&] {
            return capture_held_ && SDL_GetWindowMouseGrab(sdl_.window) && !sticky_edge_.holding;
        };
        // Escape closed the chat line above, so the pointer waits for a
        // click on the battlefield.
        sticky_move(sticky_mid_x, sticky_mid_y, 0.0F, 0.0F, 50);
        expect(
            !SDL_GetWindowMouseGrab(sdl_.window),
            "Escape on the chat line left the pointer captured"
        );
        press(SDL_EVENT_MOUSE_BUTTON_DOWN, sticky_mid_x, sticky_mid_y);
        press(SDL_EVENT_MOUSE_BUTTON_UP, sticky_mid_x, sticky_mid_y);
        match_drag_.reset();
        sticky_move(sticky_mid_x, sticky_mid_y, 0.0F, 0.0F, 50);
        expect(captured(), "Capture did not keep the pointer in a match's window");
        // Flicks and pushes far past each edge.
        const std::array<std::array<float, 4>, 4> shoves{{
            {sticky_right + 400.0F, sticky_mid_y, 9000.0F, 0.0F},
            {-400.0F, sticky_mid_y, -9000.0F, 0.0F},
            {sticky_mid_x, -400.0F, 0.0F, -9000.0F},
            {sticky_mid_x, sticky_bottom + 400.0F, 0.0F, 9000.0F},
        }};
        for (const auto& shove : shoves)
            for (int repeat = 0; repeat < 10; ++repeat) {
                sticky_move(shove[0], shove[1], shove[2], shove[3], 5);
                expect(captured(), "a push or a flick let a captured pointer leave the window");
            }
        // Each edge scrolls the view toward itself: there and back on
        // each axis, so a view already at one limit still shows both ways.
        sticky_move(sticky_right, sticky_mid_y, 0.0F, 0.0F, 50);
        pan_once();
        const auto east = match_camera_x_;
        sticky_move(0.0F, sticky_mid_y, 0.0F, 0.0F, 50);
        pan_once();
        const auto west = match_camera_x_;
        sticky_move(sticky_right, sticky_mid_y, 0.0F, 0.0F, 50);
        pan_once();
        expect(
            west < east && match_camera_x_ > west,
            "the left and right edges did not scroll a captured pointer's view"
        );
        sticky_move(sticky_mid_x, sticky_bottom, 0.0F, 0.0F, 50);
        pan_once();
        const auto south = match_camera_z_;
        sticky_move(sticky_mid_x, 0.0F, 0.0F, 0.0F, 50);
        pan_once();
        const auto north = match_camera_z_;
        sticky_move(sticky_mid_x, sticky_bottom, 0.0F, 0.0F, 50);
        pan_once();
        expect(
            north < south && match_camera_z_ > north,
            "the top and bottom edges did not scroll a captured pointer's view"
        );
        expect(captured(), "edge scrolling let a captured pointer go");
        match_camera_x_ = saved_camera_x;
        match_camera_z_ = saved_camera_z;
        scroll_clock_ = saved_clock;
        frame_time_ns_ = saved_frame;
        sticky_move(sticky_mid_x, sticky_mid_y, 0.0F, 0.0F, 50);
        // The game menu, and Settings beside it, let it go; Resume takes it
        // back.
        show_match_pause_menu();
        apply_window_edge();
        expect(!SDL_GetWindowMouseGrab(sdl_.window), "the game menu kept the pointer captured");
        resume_match_pause();
        apply_window_edge();
        expect(captured(), "closing the game menu did not capture the pointer again");
        open_engine_settings_in_match();
        apply_window_edge();
        expect(
            engine_settings_dialog() != nullptr && !SDL_GetWindowMouseGrab(sdl_.window),
            "Settings over the battlefield kept the pointer captured"
        );
        take_settings_screen_action(edge_settings::DialogAction::cancelled);
        if (match_paused_)
            resume_match_pause();
        apply_window_edge();
        expect(captured(), "closing Settings did not capture the pointer again");
        // The pause screen lets it go.
        {
            auto& game = match_->state().game;
            const uint16_t saved_run = game.sim_run_flags;
            game.sim_run_flags =
                static_cast<uint16_t>(game.sim_run_flags | oa::ui::console::kSimRunPaused);
            apply_window_edge();
            expect(!SDL_GetWindowMouseGrab(sdl_.window), "a paused game kept the pointer captured");
            game.sim_run_flags = saved_run;
            apply_window_edge();
            expect(captured(), "the game going on did not capture the pointer again");
        }
        // Escape lets it go until a click on the battlefield, which acts as
        // any click does.
        key(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE, SDL_KMOD_NONE, false);
        key(SDL_EVENT_KEY_UP, SDLK_ESCAPE, SDL_KMOD_NONE, false);
        expect(!SDL_GetWindowMouseGrab(sdl_.window), "Escape kept the pointer captured");
        // With Escape opens the game menu on, the menu closes again; the
        // pointer still waits for a click.
        if (match_paused_)
            resume_match_pause();
        apply_window_edge();
        expect(
            !SDL_GetWindowMouseGrab(sdl_.window),
            "closing the menu Escape opened captured the pointer without a click"
        );
        sticky_move(sticky_mid_x - 20.0F, sticky_mid_y, -20.0F, 0.0F, 50);
        sticky_move(sticky_right, sticky_mid_y, 20.0F, 0.0F, 50);
        expect(
            !SDL_GetWindowMouseGrab(sdl_.window),
            "moving after Escape captured the pointer without a click"
        );
        sticky_move(sticky_mid_x, sticky_mid_y, 0.0F, 0.0F, 50);
        const float acted_x = pointer_x_;
        match_drag_.reset();
        press(SDL_EVENT_MOUSE_BUTTON_DOWN, sticky_mid_x + 17.0F, sticky_mid_y + 9.0F);
        expect(captured(), "a click on the battlefield after Escape did not capture the pointer");
        expect(
            pointer_x_ != acted_x && match_drag_.has_value(),
            "the click that captured the pointer after Escape did not reach the battlefield"
        );
        press(SDL_EVENT_MOUSE_BUTTON_UP, sticky_mid_x + 17.0F, sticky_mid_y + 9.0F);
        match_drag_.reset();
        // Losing the focus lets it go; coming back does not take it.
        window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
        expect(!SDL_GetWindowMouseGrab(sdl_.window), "losing the focus kept the pointer captured");
        sticky_move(sticky_mid_x, sticky_mid_y, 0.0F, 0.0F, 50);
        window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
        sticky_move(sticky_mid_x, sticky_mid_y, 0.0F, 0.0F, 50);
        expect(
            !SDL_GetWindowMouseGrab(sdl_.window),
            "the focus coming back captured the pointer without a click"
        );
        // The click that brings the focus back captures it and is no order:
        // neither it nor its release reaches the battlefield.
        const float focused_x = pointer_x_;
        const float focused_y = pointer_y_;
        press(SDL_EVENT_MOUSE_BUTTON_DOWN, sticky_mid_x + 23.0F, sticky_mid_y + 11.0F);
        expect(captured(), "the focusing click did not capture the pointer");
        expect(
            pointer_x_ == focused_x && pointer_y_ == focused_y && !match_drag_,
            "the focusing click reached the battlefield"
        );
        press(SDL_EVENT_MOUSE_BUTTON_UP, sticky_mid_x + 23.0F, sticky_mid_y + 11.0F);
        expect(
            pointer_x_ == focused_x && pointer_y_ == focused_y && !match_drag_,
            "the focusing click's release reached the battlefield"
        );
        // The next click is the battlefield's again.
        press(SDL_EVENT_MOUSE_BUTTON_DOWN, sticky_mid_x + 23.0F, sticky_mid_y + 11.0F);
        expect(
            pointer_x_ != focused_x && match_drag_.has_value(),
            "a click after the focusing click did not reach the battlefield"
        );
        press(SDL_EVENT_MOUSE_BUTTON_UP, sticky_mid_x + 23.0F, sticky_mid_y + 11.0F);
        match_drag_.reset();
        expect(captured(), "a click on the battlefield let the captured pointer go");
        // A click well after the focus came back, or after a key, is the
        // player's own: it captures the pointer and reaches the battlefield.
        const auto later_click = [&](bool key_between, std::string_view what) {
            window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
            const uint64_t back_ns = SDL_GetTicksNS();
            SDL_Event gained{};
            gained.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
            gained.window.windowID = SDL_GetWindowID(sdl_.window);
            gained.window.timestamp = back_ns;
            dispatch_event(gained, running);
            if (key_between) {
                key(SDL_EVENT_KEY_DOWN, SDLK_LSHIFT, SDL_KMOD_LSHIFT, false);
                key(SDL_EVENT_KEY_UP, SDLK_LSHIFT, SDL_KMOD_NONE, false);
            }
            sticky_move(sticky_mid_x, sticky_mid_y, 0.0F, 0.0F, 50);
            const float before_x = pointer_x_;
            SDL_Event later{};
            later.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            later.button.windowID = SDL_GetWindowID(sdl_.window);
            later.button.button = SDL_BUTTON_LEFT;
            later.button.down = true;
            later.button.clicks = 1;
            later.button.x = sticky_mid_x + 29.0F;
            later.button.y = sticky_mid_y + 13.0F;
            later.button.timestamp =
                back_ns +
                (key_between ? 0ULL : 600ULL * frame_pacing::kNanosecondsPerSecond / 1000ULL);
            dispatch_event(later, running);
            expect(
                captured() && pointer_x_ != before_x && match_drag_.has_value(),
                std::string(what) + " did not capture the pointer and reach the battlefield"
            );
            press(SDL_EVENT_MOUSE_BUTTON_UP, sticky_mid_x + 29.0F, sticky_mid_y + 13.0F);
            match_drag_.reset();
        };
        later_click(false, "a click 600 ms after the focus came back");
        later_click(true, "a click after a key with the focus back");
        std::cout << "frontend controls check: Capture keeps the pointer in a match's window "
                     "through pushes and flicks, and lets it go in menus, after Escape and "
                     "without the focus\n";

        // Sticky, at Edge stickiness 1.0.
        engine_settings_state().current.window_edge = edge_settings::WindowEdge::sticky;
        engine_settings_state().current.edge_stickiness = edge_settings::default_edge_stickiness;
        const auto held = [&] {
            return sticky_edge_.holding && !SDL_GetWindowMouseGrab(sdl_.window);
        };
        float sticky_x = sticky_mid_x;
        sticky_move(sticky_x, sticky_mid_y, 0.0F, 0.0F, 50);
        expect(held(), "the pointer at the middle of a match window was not held");
        expect(!capture_held_, "Sticky kept the pointer captured");
        while (sticky_x + 10.0F < sticky_right) {
            sticky_move(sticky_x + 10.0F, sticky_mid_y, 10.0F, 0.0F, 50);
            sticky_x += 10.0F;
        }
        sticky_move(sticky_right, sticky_mid_y, sticky_right - sticky_x, 0.0F, 50);
        expect(held(), "a slow move onto the edge let the pointer go");
        for (int rest = 0; rest < 40; ++rest)
            sticky_move(sticky_right, sticky_mid_y, 0.0F, 0.0F, 50);
        expect(held(), "resting on the edge let the pointer go");
        // A leave forgets the place. While the pointer rests on the edge the
        // view still scrolls from that last pixel.
        window_event(SDL_EVENT_WINDOW_MOUSE_LEAVE);
        expect(
            match_pointer_known_ && match_pointer_x_ > sticky_right - 1.0F &&
                match_pointer_x_ < sticky_right + 1.0F,
            "a leave while the pointer rests on the edge hid it from edge scrolling"
        );
        const auto sticky_camera_x = match_camera_x_;
        pan_once();
        expect(
            match_camera_x_ > sticky_camera_x,
            "the view did not scroll while the pointer rested on the edge"
        );
        match_camera_x_ = saved_camera_x;
        match_camera_z_ = saved_camera_z;
        scroll_clock_ = saved_clock;
        frame_time_ns_ = saved_frame;
        // Past the last pixel, and put back on it. Three such steps of 100
        // are 1.0's push of 300.
        sticky_move(sticky_right + 100.0F, sticky_mid_y, 0.0F, 0.0F, 50);
        float placed_x = 0.0F;
        float placed_y = 0.0F;
        std::ignore = SDL_GetMouseState(&placed_x, &placed_y);
        expect(
            held() && placed_x > sticky_right - 1.0F && placed_x < sticky_right + 1.0F,
            "the pointer was not put back on the edge"
        );
        expect(
            match_pointer_known_ && match_pointer_x_ > sticky_right - 1.0F &&
                match_pointer_x_ < sticky_right + 1.0F,
            "edge scrolling does not see the pointer on the edge"
        );
        sticky_move(sticky_right + 100.0F, sticky_mid_y, 0.0F, 0.0F, 50);
        expect(held(), "a push under the threshold let the pointer go");
        sticky_move(sticky_right + 100.0F, sticky_mid_y, 0.0F, 0.0F, 50);
        expect(!sticky_edge_.holding, "a push past the threshold did not let the pointer go");
        expect(!SDL_GetWindowMouseGrab(sdl_.window), "letting the pointer go grabbed the window");
        const float sticky_near = sticky_right - 23.0F;
        sticky_move(sticky_near, sticky_mid_y, sticky_near - sticky_right, 0.0F, 50);
        expect(!sticky_edge_.holding, "coming back within 24 points of the edge held the pointer");
        const float sticky_rearm = sticky_right - 24.0F;
        sticky_move(sticky_rearm, sticky_mid_y, sticky_rearm - sticky_near, 0.0F, 50);
        expect(held(), "moving 24 points in from the edge did not hold the pointer");
        // Twice as firm, the same three steps hold; three more let go.
        engine_settings_state().current.edge_stickiness =
            2 * edge_settings::default_edge_stickiness;
        for (int step = 0; step < 3; ++step)
            sticky_move(sticky_right + 100.0F, sticky_mid_y, 0.0F, 0.0F, 50);
        expect(held(), "Edge stickiness 2.0 let go of 1.0's push");
        for (int step = 0; step < 3; ++step)
            sticky_move(sticky_right + 100.0F, sticky_mid_y, 0.0F, 0.0F, 50);
        expect(!sticky_edge_.holding, "Edge stickiness 2.0 held twice 1.0's push");
        // 0.0 never holds.
        engine_settings_state().current.edge_stickiness = edge_settings::lowest_edge_stickiness;
        sticky_move(sticky_mid_x, sticky_mid_y, 0.0F, 0.0F, 50);
        sticky_move(sticky_right, sticky_mid_y, sticky_right - sticky_mid_x, 0.0F, 50);
        expect(!sticky_edge_.holding, "Edge stickiness 0.0 held the pointer");
        engine_settings_state().current.edge_stickiness = saved_level;

        // Free never holds and never grabs.
        engine_settings_state().current.window_edge = edge_settings::WindowEdge::free;
        sticky_x = sticky_mid_x;
        sticky_move(sticky_x, sticky_mid_y, 0.0F, 0.0F, 50);
        while (sticky_x + 10.0F < sticky_right) {
            sticky_move(sticky_x + 10.0F, sticky_mid_y, 10.0F, 0.0F, 50);
            sticky_x += 10.0F;
        }
        sticky_move(sticky_right + 40.0F, sticky_mid_y, 40.0F, 0.0F, 50);
        expect(!sticky_edge_.holding, "Window edge Free held the pointer");
        expect(!SDL_GetWindowMouseGrab(sdl_.window), "Window edge Free grabbed the pointer");
        engine_settings_state().current.window_edge = saved_edge;
        sticky_move(sticky_mid_x, sticky_mid_y, 0.0F, 0.0F, 50);
        if (sticky_started_full)
            alt_enter(SDLK_RETURN, 0);
        std::cout << "frontend controls check: Sticky puts the pointer back on the window's edge "
                     "until a push as firm as Edge stickiness, and Free leaves it free\n";
    }

    // The in-match Options' Visuals draws the same Screen Size. Choosing one
    // and OK (PREV) stores it and applies it: the window and the match take
    // the size. The pause menu then closes, so the checks below see the match.
    {
        int options_width = 0;
        int options_height = 0;
        (void)SDL_GetWindowSize(sdl_.window, &options_width, &options_height);
        const bool options_full_screen = full_screen();
        if (options_full_screen)
            alt_enter(SDLK_RETURN, 0);
        const auto shown_size = EngineSettingsState::screen_size_now(*this);
        auto picked_size = offered.front();
        for (const auto size : offered)
            if (size != shown_size) {
                picked_size = size;
                break;
            }
        const auto chosen_words = size_text(picked_size);
        const auto chosen_stored =
            std::to_string(picked_size.width) + 'x' + std::to_string(picked_size.height);
        show_match_pause_menu();
        activate_pause_gadget("PREFS");
        activate_pause_gadget("VISUALS");
        const auto hud_label = [this](std::string_view name) {
            std::string text;
            if (!match_hud_)
                return text;
            for (const auto& gadget : match_hud_->layout.gadgets) {
                if (gadget.common.name != name)
                    continue;
                if (const auto* label =
                        std::get_if<oa::ui::gui_layout::LabelFields>(&gadget.fields))
                    text = label->text;
            }
            return text;
        };
        const auto hud_shows = [this](std::string_view name) {
            if (!match_hud_)
                return false;
            for (const auto& gadget : match_hud_->layout.gadgets)
                if (gadget.common.name == name && gadget.common.active != 0)
                    return true;
            return false;
        };
        require(
            hud_shows("VIDSLDR") && hud_shows("VIDVAL"), "the in-match Visuals draw no Screen Size"
        );
        drag_check_knob("VIDSLDR", -400);
        for (int options_step = 0; options_step < 400 && hud_label("VIDVAL") != chosen_words;
             ++options_step)
            click_check_arrow("VIDSLDR", true);
        expect(
            hud_label("VIDVAL") == chosen_words,
            "the in-match Screen Size shows " + hud_label("VIDVAL") + ", not " + chosen_words
        );
        activate_pause_gadget("PREV");
        settle_window();
        expect(
            stored_size() == chosen_stored &&
                engine_settings_state().current.screen_size == picked_size &&
                match_layout_.width == picked_size.width &&
                match_layout_.height == picked_size.height && window_points() == chosen_stored,
            "OK on the in-match Screen Size left the setting at " + stored_size() +
                ", the window at " + window_points() + " and the match at " +
                std::to_string(match_layout_.width) + 'x' + std::to_string(match_layout_.height)
        );
        std::cout << "frontend controls check: the in-match Options' Screen Size applies "
                  << chosen_words << '\n';
        EngineSettingsState::choose_screen_size(
            *this, oa::ui::engine_settings::desktop_screen_size
        );
        save_preferences();
        require(SDL_SetWindowSize(sdl_.window, options_width, options_height), SDL_GetError());
        settle_window();
        if (match_paused_)
            resume_match_pause();
        if (options_full_screen)
            alt_enter(SDLK_RETURN, 0);
    }

    // A Screen size applied in a match: the window takes it at once and the
    // match is laid out at it. In full screen on the display's desktop mode
    // (the dummy driver offers no other) the match is drawn at the size and
    // scaled to the screen, letterboxed, the pointer in the black bars
    // resting on the frame's edge; a size chosen there is the window's once
    // it leaves full screen; Desktop draws at the screen's own size again.
    {
        const bool started_full_screen = full_screen();
        if (started_full_screen)
            alt_enter(SDLK_RETURN, 0);
        int before_width = 0;
        int before_height = 0;
        (void)SDL_GetWindowSize(sdl_.window, &before_width, &before_height);
        const uint32_t tick = match_timing_.tick;
        const std::string scaled_text =
            std::to_string(kChosenSize.width) + 'x' + std::to_string(kChosenSize.height);
        EngineSettingsState::take_screen_size(*this, kChosenSize);
        settle();
        expect(
            window_points() == scaled_text && follows_window() &&
                match_layout_.width == kChosenSize.width &&
                match_layout_.height == kChosenSize.height,
            "a Screen size of " + scaled_text + " in a match left the window at " +
                window_points() + " and the match at " + std::to_string(match_layout_.width) + 'x' +
                std::to_string(match_layout_.height)
        );
        alt_enter(SDLK_RETURN, 0);
        expect(full_screen(), "Alt+Enter did not switch a sized window to full screen");
        int logical_width = 0;
        int logical_height = 0;
        SDL_RendererLogicalPresentation presentation = SDL_LOGICAL_PRESENTATION_DISABLED;
        (void)SDL_GetRenderLogicalPresentation(
            sdl_.renderer, &logical_width, &logical_height, &presentation
        );
        SDL_FRect drawn{};
        (void)SDL_GetRenderLogicalPresentationRect(sdl_.renderer, &drawn);
        expect(
            scaled_frame_size().x == kChosenSize.width &&
                match_layout_.width == kChosenSize.width &&
                match_layout_.height == kChosenSize.height && logical_width == kChosenSize.width &&
                logical_height == kChosenSize.height &&
                presentation != SDL_LOGICAL_PRESENTATION_DISABLED,
            "full screen did not draw the match at " + scaled_text + " and scale it: " +
                std::to_string(match_layout_.width) + 'x' + std::to_string(match_layout_.height)
        );
        snapshot_window("scaled-frame");
        // The pointer above the frame, in the black bar, rests on its top
        // row; one left of it on its first column.
        const auto window_motion = [&](float x, float y) {
            SDL_Event motion{};
            motion.type = SDL_EVENT_MOUSE_MOTION;
            motion.motion.windowID = SDL_GetWindowID(sdl_.window);
            motion.motion.x = x;
            motion.motion.y = y;
            dispatch_event(motion, running);
        };
        float density = SDL_GetWindowPixelDensity(sdl_.window);
        if (!(density > 0.0F))
            density = 1.0F;
        if (drawn.y >= 2.0F) {
            window_motion((drawn.x + drawn.w / 2.0F) / density, 0.0F);
            expect(
                match_pointer_y_ == 0.0F && match_pointer_x_ >= 0.0F &&
                    match_pointer_x_ < static_cast<float>(kChosenSize.width),
                "the pointer above the scaled frame did not rest on its top row"
            );
        }
        if (drawn.x >= 2.0F) {
            window_motion(0.0F, (drawn.y + drawn.h / 2.0F) / density);
            expect(
                match_pointer_x_ == 0.0F,
                "the pointer left of the scaled frame did not rest on its first column"
            );
        }
        // A size chosen in full screen, then the window it leaves at.
        constexpr oa::ui::engine_settings::ScreenSize kFullScreenSize{800, 600};
        EngineSettingsState::take_screen_size(*this, kFullScreenSize);
        expect(
            full_screen() && match_layout_.width == kFullScreenSize.width &&
                match_layout_.height == kFullScreenSize.height,
            "a Screen size of 800x600 in full screen did not draw the match at it"
        );
        alt_enter(SDLK_RETURN, 0);
        settle();
        expect(
            !full_screen() && window_points() == "800x600" && follows_window(),
            "the window left full screen at " + window_points() + ", not 800x600"
        );
        // Desktop in full screen draws at the screen's own size.
        alt_enter(SDLK_RETURN, 0);
        EngineSettingsState::take_screen_size(*this, oa::ui::engine_settings::desktop_screen_size);
        expect(
            full_screen() && scaled_frame_size().x == 0 && follows_window(),
            "Desktop in full screen did not draw the match at the screen's size"
        );
        alt_enter(SDLK_RETURN, 0);
        expect(
            match_ && match_timing_.tick == tick, "applying a Screen size ran or reset the game"
        );
        std::cout << "frontend controls check: a Screen size in a match: a " << scaled_text
                  << " window, full screen drawn at " << scaled_text << " in "
                  << static_cast<int>(drawn.w) << 'x' << static_cast<int>(drawn.h)
                  << ", 800x600 chosen in full screen and Desktop\n";
        // The check's file and window as they were.
        EngineSettingsState::choose_screen_size(
            *this, oa::ui::engine_settings::desktop_screen_size
        );
        save_preferences();
        require(SDL_SetWindowSize(sdl_.window, before_width, before_height), SDL_GetError());
        settle();
        if (started_full_screen)
            alt_enter(SDLK_RETURN, 0);
    }
    // SDL's dummy video driver gives its window the focus, so there full
    // screen held the pointer.
    const char* video_driver = SDL_GetCurrentVideoDriver();
    if (video_driver != nullptr && std::string_view(video_driver) == "dummy")
        expect(pointer_kept_in_full_screen, "full screen never kept the pointer on the window");

    // A saved game leaves the skirmish setup as it was, even when the
    // preferences are saved while it runs, as the in-game options save them:
    // the rows show the same metal and energy, the preferences keep them,
    // and the next Start gives each player its row's.
    const entry::SkirmishSettings setup_before_save = skirmish_settings_;
    const fs::path saved_game = "frontend-controls.sav";
    if (!save_match_game(
            saved_game,
            oa::data::persist::command_line_description,
            oa::data::persist::command_line_game_id
        ))
        fail("could not save the match: " + status_);
    if (!load_saved_game(saved_game) || !match_)
        fail("could not load the saved match: " + status_);
    save_preferences();
    leave_match();
    load(Screen::main_menu);
    exercise_click(menu::resource_name(menu::Button::single_player));
    require(screen_ == Screen::single_player, "did not reach SINGLE.GUI after a saved game");
    click(entry::resource_name(entry::Button::skirmish));
    require(screen_ == Screen::skirmish, "Skirmish did not open SKIRMISH.GUI after a saved game");
    snapshot("skirmish-after-saved-game");
    expect(
        skirmish_settings_.slot_count == setup_before_save.slot_count,
        "SKIRMISH.GUI shows " + std::to_string(skirmish_settings_.slot_count) +
            " rows after a saved game, not " + std::to_string(setup_before_save.slot_count)
    );
    expect_row_resources(setup_before_save, "after a saved game");
    for (int32_t row = 0; row < setup_before_save.slot_count; ++row) {
        const auto& kept = setup_before_save.slots[static_cast<std::size_t>(row)];
        for (const auto& [resource, amount] :
             {std::pair{std::string_view("Metal"), kept.metal},
              std::pair{std::string_view("Energy"), kept.energy}}) {
            const auto preference = "Player" + std::to_string(row) + std::string(resource);
            const auto stored = read_number(init::skirmish_section, preference);
            expect(
                stored && *stored == static_cast<uint32_t>(amount),
                preference + " is not stored as " + std::to_string(amount) + " after a saved game"
            );
        }
    }
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    require(screen_ == Screen::match && match_, "Start did not enter a match after a saved game");
    for (std::size_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        const auto& kept = setup_before_save.slots[player];
        if (kept.controller == entry::controller::disabled)
            continue;
        const auto& record = match_->state().game.players[player];
        expect(
            record.metal == static_cast<float>(kept.metal) &&
                record.energy == static_cast<float>(kept.energy),
            "player " + std::to_string(player) + " starts with " +
                std::to_string(static_cast<int32_t>(record.metal)) + " metal and " +
                std::to_string(static_cast<int32_t>(record.energy)) + " energy, not " +
                std::to_string(kept.metal) + " and " + std::to_string(kept.energy)
        );
    }
    std::cout << "frontend controls check: after a saved game the " << setup_before_save.slot_count
              << " rows keep their metal and energy, and Start gives each player its row's\n";

    if (!problems.empty()) {
        std::string report;
        for (const auto& problem : problems)
            report += "\n  " + problem;
        fail(
            std::to_string(problems.size()) +
            " controls do not show their setting, help or choice:" + report
        );
    }
    std::cout << "frontend controls check: every control shows its setting, help and choice\n";
}

} // namespace oa::app
