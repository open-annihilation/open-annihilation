// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-kit-gallery: the UI kit gallery opened from Settings › Developer
// over the main menu on 640x480, 1280x720 and 1920x1080 windows, in the
// classic look and then the modern one. It is Compact at 1x on the first
// and Large at 1x on the others, where Auto steps down from 2x at
// 1920x1080. At each it covers the window, shows exactly the gallery drawn
// offscreen at that class and scale in that look over the darkened main
// menu, takes the focus to every control it declares once in Tab's order,
// and Escape brings Settings › Developer back.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/automation_host.hpp"
#include "oa/app/runtime.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/gallery.hpp"
#include "oa/ui/kit/layout.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

namespace {

namespace settings = oa::ui::engine_settings;
namespace kit = oa::ui::kit;
namespace layout = oa::ui::display_layout;

/// The modifier the settings' shortcut takes on this platform.
#ifdef SDL_PLATFORM_MACOS
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_GUI;
#else
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_CTRL;
#endif

/// Where the pointer rests while the window's frame is read: the main
/// menu's picture's bottom-right pixel, over no control of the gallery at
/// any of the windows.
constexpr layout::Point kRestingPointer{kCanvasWidth - 1, kCanvasHeight - 1};

/// The modal screens with a backdrop over the main menu while the gallery
/// shows: Settings and the gallery.
constexpr uint32_t kDarkenings = 2;

/// One window the gallery is opened on, and the size class and scale it
/// takes there.
struct GalleryWindow {
    int width{};                 ///< the window's width, in pixels
    int height{};                ///< the window's height, in pixels
    kit::SizeClass size_class{}; ///< the gallery's class
    int32_t scale{};             ///< window pixels a point
};

constexpr std::array<GalleryWindow, 3> kWindows{{
    {kCanvasWidth, kCanvasHeight, kit::SizeClass::compact, 1},
    {1280, 720, kit::SizeClass::large, 1},
    {1920, 1080, kit::SizeClass::large, 1},
}};

/// Stops the check with a reason unless a condition holds.
///
/// @param condition what must hold
/// @param what what went wrong otherwise
void require(bool condition, std::string_view what) {
    if (!condition)
        throw std::runtime_error("kit gallery check: " + std::string(what));
}

/// The looks the gallery is checked in, in order.
constexpr std::array<settings::Look, 2> kLooks{settings::Look::classic, settings::Look::modern};

/// Returns a look's name in the check's lines.
///
/// @param look the look
/// @return "classic" or "modern"
std::string_view look_name(settings::Look look) noexcept {
    return look == settings::Look::modern ? "modern" : "classic";
}

/// Returns the path of a snapshot of the gallery on a window in a look:
/// <stem>-kit-gallery-<W>x<H>.ppm beside --snapshot in the classic look,
/// <stem>-kit-gallery-modern-<W>x<H>.ppm in the modern one.
///
/// @param snapshot the --snapshot path
/// @param window the window
/// @param look the look
/// @return the snapshot's path
std::filesystem::path gallery_snapshot(
    const std::filesystem::path& snapshot, const GalleryWindow& window, settings::Look look
) {
    const std::string modern = look == settings::Look::modern ? "modern-" : "";
    return snapshot.parent_path() /
           (snapshot.stem().string() + "-kit-gallery-" + modern + std::to_string(window.width) +
            "x" + std::to_string(window.height) + ".ppm");
}

/// Returns the first part of the dialog's layout that shows a text.
///
/// @param parts the dialog's layout
/// @param control the part's control
/// @param text the text it shows; empty for any
/// @return the part; null when the layout has none
const settings::LayoutPart*
part_of(const std::vector<settings::LayoutPart>& parts, int32_t control, std::string_view text) {
    for (const settings::LayoutPart& part : parts)
        if (part.control == control && (text.empty() || part.text == text))
            return &part;
    return nullptr;
}

} // namespace

void Runtime::check_kit_gallery() {
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        throw std::runtime_error("kit gallery check: needs the SDL presenter");
    // Each run starts from an empty preferences file, with the settings'
    // defaults in effect; the gallery itself changes none.
    if (!options_.preferences_file)
        throw std::runtime_error("kit gallery check: needs --preferences-file");
    preference_values_.clear();
    oa::platform::preferences::save(preference_path_, preference_values_);
    init::load_preferences(state_, skirmish_settings_, preferences_, *this);
    load_engine_settings();
    // An extension's overlay may draw over any part of the main menu and
    // take its input first, so the extensions' overlays are set aside while
    // the check holds the window to the gallery's own drawing, and put back
    // when it ends.
    set_extension_overlays_aside(true);
    // Every frame read back holds the whole window, which the backdrops
    // darken too.
    oa_layer().read_whole_window(true);

    struct PutBack {
        Runtime& runtime;

        ~PutBack() {
            runtime.oa_layer().read_whole_window(false);
            runtime.set_extension_overlays_aside(false);
        }
    } put_back{*this};

    require(gamma_identity_, "the check compares frames at a display gamma that changes no colour");
    const auto previous_tick = fake_frontend_tick_;
    fake_frontend_tick_ = 1000U;

    const auto tap = [this](SDL_Keycode code, SDL_Keymod modifiers = SDL_KMOD_NONE) {
        bool running = true;
        for (const bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = SDL_GetWindowID(sdl_.window);
            event.key.key = code;
            event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
            event.key.mod = modifiers;
            event.key.down = down;
            dispatch_event(event, running);
        }
        require(running, "a key ended the run");
    };
    // Clicks a window pixel, as the pointer there would.
    const auto click_window = [this](layout::Point pixel) {
        for (const SDL_EventType type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event = window_pointer_event(
                sdl_.window,
                sdl_.renderer,
                type,
                pixel,
                type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
            );
            bool running = true;
            dispatch_event(event, running);
            require(running, "a click in the window ended the run");
        }
    };
    // Clicks the middle of one of the dialog's parts, where the window shows it.
    const auto click_part = [&](const settings::LayoutPart* part, std::string_view what) {
        require(part != nullptr, "the dialog shows no " + std::string(what));
        click_window(layer_window_pixel(
            oa_layer().placement_of("settings"),
            {part->rect.x + part->rect.width / 2, part->rect.y + part->rect.height / 2}
        ));
    };
    const auto sparks = menu_sparks_;
    // The window's frame, drawn from the same sparks each time.
    const auto presented_frame = [&] {
        renderer::Surface presented;
        menu_sparks_ = sparks;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        return presented;
    };

    const settings::EngineSettings kept_settings = engine_settings();
    for (const settings::Look look : kLooks) {
        // The look in effect, as Settings › User Interface › Look puts it.
        settings::EngineSettings chosen = kept_settings;
        chosen.look = look;
        apply_engine_settings(chosen);
        require(
            engine_settings_fonts() != nullptr &&
                kit::modern_look(*engine_settings_fonts()) == (look == settings::Look::modern),
            "the " + std::string(look_name(look)) + " look did not take effect"
        );
        for (const GalleryWindow& window : kWindows) {
            const std::string size =
                std::to_string(window.width) + "x" + std::to_string(window.height);
            const std::string on =
                " on the " + size + " window in the " + std::string(look_name(look)) + " look";
            // The window at its size, the main menu, and Settings.
            require(
                SDL_SetWindowSize(sdl_.window, window.width, window.height) &&
                    SDL_SyncWindow(sdl_.window),
                "the window did not take " + size
            );
            apply_output_mode();
            load(Screen::main_menu);
            require(screen_ == Screen::main_menu, "the main menu did not open" + on);
            send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
            // The window's frame with no OA screen open, which the backdrops darken.
            const renderer::Surface closed = presented_frame();
            tap(SDLK_COMMA, kShortcutModifier);
            require(
                engine_settings_dialog() != nullptr && oa_layer().find("settings") != nullptr,
                "the shortcut did not open Settings" + on
            );

            // Developer's entry, then SHOW, where the window shows them.
            {
                const auto parts = settings::dialog_layout(*engine_settings_dialog());
                click_part(
                    part_of(parts, settings::page_control(settings::Page::developer), "Developer"),
                    "Developer entry"
                );
            }
            require(
                engine_settings_dialog() != nullptr &&
                    engine_settings_dialog()->page == settings::Page::developer,
                "a click on Developer's entry did not show Developer" + on
            );
            {
                const auto parts = settings::dialog_layout(*engine_settings_dialog());
                const settings::LayoutPart* show = nullptr;
                for (const settings::LayoutPart& part : parts)
                    // SHOW, or Show in title case.
                    if ((part.text == "SHOW" || part.text == "Show") &&
                        part.control != settings::no_control)
                        show = &part;
                click_part(show, "SHOW on UI kit gallery");
            }

            // The gallery on top, at the class and scale this window gives,
            // over the whole window.
            auto* gallery = dynamic_cast<GalleryScreen*>(oa_layer().top());
            require(
                gallery != nullptr && gallery->name() == GalleryScreen::screen_name,
                "SHOW did not open the gallery on top" + on
            );
            const LayerPlacement placed = oa_layer().placement_of(GalleryScreen::screen_name);
            require(
                gallery->frame().size_class == window.size_class &&
                    gallery->frame().scale_percent == window.scale * 100 &&
                    placed.scale_percent == window.scale * 100,
                "the gallery is not " + std::string(size_class_name(window.size_class)) + ", " +
                    std::to_string(window.scale) + "x" + on
            );
            require(
                placed.shown.x == 0 && placed.shown.y == 0 && placed.shown.width == window.width &&
                    placed.shown.height == window.height &&
                    placed.points_width * window.scale == window.width &&
                    placed.points_height * window.scale == window.height,
                "the gallery does not cover the window" + on
            );
            // Its controls reach the automation endpoint by the kit's names, in
            // its own dialog.
            {
                std::vector<AutomationControl> listed;
                oa_layer().automation_controls(listed);
                for (const std::string_view name :
                     {"oa.gallery.close", "oa.gallery.toggle.1.on", "oa.gallery.drop-down.1"}) {
                    bool found = false;
                    for (const AutomationControl& control : listed)
                        found = found || (control.name == name && control.dialog == "oa.gallery");
                    require(found, "the endpoint does not list " + std::string(name) + on);
                }
            }

            // The window shows the gallery as drawn offscreen at its class and
            // scale, over the main menu darkened by Settings' backdrop and its own.
            send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
            renderer::Surface drawing = layer_drawing(
                placed.points_width,
                placed.points_height,
                placed.scale_percent,
                oa_layer().screen_fonts(),
                oa_layer().smooth_fonts(),
                engine_settings_icon(),
                [gallery](const kit::Canvas& canvas) {
                    kit::draw_gallery(canvas, gallery->layout());
                }
            );
            apply_gamma_rgb(drawing.rgb.data(), drawing.rgb.size() / 3U, 3);
            const renderer::Surface expected =
                expected_layer_frame(closed, drawing, placed, kDarkenings);
            const renderer::Surface shown = presented_frame();
            const std::size_t differing = layer_differences(
                shown, expected, window_pointer(oa_layer().view(), kRestingPointer)
            );
            if (differing != 0 && !options_.snapshot.empty()) {
                const auto stem = options_.snapshot.parent_path() / options_.snapshot.stem();
                const std::string modern = look == settings::Look::modern ? "modern-" : "";
                write_ppm(
                    stem.string() + "-kit-gallery-expected-" + modern + size + ".ppm", expected
                );
            }
            // The snapshot of the gallery as the window shows it.
            if (!options_.snapshot.empty())
                write_ppm(gallery_snapshot(options_.snapshot, window, look), shown);
            require(
                differing == 0,
                "the window does not show the gallery" + on + ": " + std::to_string(differing) +
                    " pixels differ"
            );

            // Tab takes the focus to every control the gallery declares, once
            // each, in its order.
            const std::vector<kit::ControlId> order = gallery->layout().list.tab_order;
            std::set<kit::ControlId> focused;
            for (const kit::ControlId id : order) {
                tap(SDLK_TAB);
                require(oa_layer().top() == gallery, "Tab took the gallery off" + on);
                const kit::Interaction now = gallery->interaction();
                require(
                    now.focus_shown && now.focused == id,
                    "Tab did not take the focus to control " + std::to_string(id) + on
                );
                focused.insert(now.focused);
            }
            std::size_t focusable = 0;
            for (const kit::Control& control : gallery->layout().list.controls)
                if (control.focusable) {
                    ++focusable;
                    require(
                        focused.contains(control.id), control.name + " never took the focus" + on
                    );
                }
            require(
                !order.empty() && focused.size() == order.size() && focusable == order.size(),
                "Tab did not focus each of the gallery's controls once" + on
            );

            // Escape takes the gallery off, back to Settings › Developer.
            tap(SDLK_ESCAPE);
            require(
                oa_layer().find(GalleryScreen::screen_name) == nullptr &&
                    oa_layer().top() == oa_layer().find("settings") &&
                    engine_settings_dialog() != nullptr &&
                    engine_settings_dialog()->page == settings::Page::developer,
                "Escape did not bring Settings › Developer back" + on
            );

            // The line, then Settings closed for the next window.
            std::cout << "kit gallery check: " << size_class_name(window.size_class) << ", "
                      << window.scale << "x, on the " << size << " window, " << look_name(look)
                      << " look\n";
            tap(SDLK_ESCAPE);
            require(engine_settings_dialog() == nullptr, "Escape did not close Settings" + on);
            oa_layer().forget_latched_key();
        }
    }
    apply_engine_settings(kept_settings);
    require(
        SDL_SetWindowSize(sdl_.window, kCanvasWidth, kCanvasHeight) && SDL_SyncWindow(sdl_.window),
        "the window did not take 640x480 back"
    );
    apply_output_mode();
    fake_frontend_tick_ = previous_tick;
    std::cout << "kit gallery check: passed\n";
}

} // namespace oa::app
