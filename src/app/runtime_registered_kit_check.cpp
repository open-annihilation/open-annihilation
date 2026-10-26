// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-registered-kit: a settings page, a kit screen, a notice and a
// prompt registered from inside the engine. The page is under its owner's
// divider at three window sizes, the screen opens by name, and the notice,
// the prompt and the saves notice show one at a time.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "user_folder_state.hpp"

#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/rows.hpp"
#include "oa/ui/kit/theme.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

namespace settings = oa::ui::engine_settings;
namespace kit = oa::ui::kit;
namespace layout = oa::ui::display_layout;
namespace renderer = oa::ui::frontend_renderer;

namespace {

/// The modifier the settings' shortcut takes on this platform.
#ifdef SDL_PLATFORM_MACOS
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_GUI;
#else
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_CTRL;
#endif

/// The registered page's rows. They outlive the dialog that lists them.
constexpr std::size_t kPageRows = 2;

/// What OK told the page, and how many times.
struct Applied {
    int32_t calls{};     ///< how many times applied ran
    std::string key{};   ///< the last key
    std::string value{}; ///< the last value
};

/// What a prompt's answer told its callback.
struct Answers {
    int32_t calls{};    ///< how many times answered ran
    int32_t button{-2}; ///< the last button
    uint32_t id{};      ///< the last prompt's id
};

/// The kit screen's model: how many times it has closed.
struct ScreenModel {
    int32_t closed{}; ///< closed calls
};

/// Throws when a step of the check failed.
///
/// @param ok the step held
/// @param what the step
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("registered kit check: " + std::string(what));
}

/// Records one key OK saved.
///
/// @param state the Applied the page was registered with
/// @param key the key
/// @param value the value stored
void on_applied(void* state, const char* key, const char* value) {
    auto* seen = static_cast<Applied*>(state);
    ++seen->calls;
    seen->key = key != nullptr ? key : "";
    seen->value = value != nullptr ? value : "";
}

/// Records one answer.
///
/// @param user the Answers the prompt was queued with
/// @param id the prompt's id
/// @param button the button, or -1 when the run drops the prompt
void on_answered(void* user, uint32_t id, int32_t button) {
    auto* seen = static_cast<Answers*>(user);
    ++seen->calls;
    seen->id = id;
    seen->button = button;
}

/// Counts a close of the kit screen.
///
/// @param model the screen's model
void on_closed(void* model) {
    ++static_cast<ScreenModel*>(model)->closed;
}

/// Closes the kit screen when CLOSE is pressed, and leaves every other press.
///
/// @param model the screen's model, unused
/// @param action the press or the key
/// @return 2 for CLOSE; 0 otherwise
int on_act(void* model, const KitScreenAction& action) {
    static_cast<void>(model);
    return action.control == 4 ? 2 : 0;
}

/// Lays the kit screen out: a heading, three list rows and CLOSE.
///
/// @param model the screen's model, unused
/// @param frame the frame the layer gives the screen
/// @return the display list
kit::DisplayList lay_screen(void* model, const kit::Frame& frame) {
    static_cast<void>(model);
    kit::DisplayList list;
    const int32_t width = frame.area.width > 0 ? frame.area.width : 1;
    const int32_t height = frame.area.height > 0 ? frame.area.height : 1;
    const int32_t wide = width > 36 ? width - 24 : width;
    kit::Item face;
    face.role = kit::Role::fill;
    face.rect = {0, 0, width, height};
    face.colour = kit::colour::panel;
    list.items.push_back(std::move(face));
    kit::HeadingLook heading;
    heading.text = "Section";
    kit::Item title;
    title.role = kit::Role::heading;
    title.rect = {12, 8, wide, kit::compact_metrics.regular_line};
    title.look = std::move(heading);
    list.items.push_back(std::move(title));
    const int32_t row_height = kit::compact_metrics.list_row_height;
    int32_t top = 8 + kit::compact_metrics.regular_line + 8;
    const std::array<const char*, 3> titles{"One", "Two", "Three"};
    const std::array<const char*, 3> names{
        "check.screen.one", "check.screen.two", "check.screen.three"
    };
    for (int32_t index = 0; index < 3; ++index) {
        kit::ListRowLook row;
        row.title = titles[static_cast<std::size_t>(index)];
        kit::add_list_row(
            list,
            {12, top, wide, row_height},
            row,
            index + 1,
            names[static_cast<std::size_t>(index)]
        );
        top += row_height + 2;
    }
    kit::ButtonLook close_look;
    close_look.caption = "CLOSE";
    close_look.style = kit::ButtonStyle::accent;
    kit::add_button(
        list, {12, top, 80, kit::compact_metrics.button_height}, close_look, 4, "check.screen.close"
    );
    list.tab_order = {1, 2, 3, 4};
    return list;
}

/// Returns the page's rows.
///
/// @return the two rows, alive for the run
const kit::PreferenceRow* page_rows() {
    static const std::string_view ids[] = {"quiet", "loud"};
    static const std::string_view captions[] = {"Quiet", "Loud"};
    static const kit::PreferenceRow rows[] = {
        kit::preference_toggle("toggle", "Toggle", "check.toggle", "1"),
        kit::preference_choice("mode", "Mode", "check.mode", "quiet", ids, captions),
    };
    return rows;
}

/// One window the page and the screen are shown at.
struct WindowCase {
    int width{};                 ///< the window's width
    int height{};                ///< the window's height
    kit::SizeClass size_class{}; ///< the class the layer reports
    int32_t scale{};             ///< window pixels a point
    bool write{};                ///< click the toggle Off and OK
};

} // namespace

void Runtime::check_registered_kit() {
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        throw std::runtime_error("registered kit check: needs the SDL presenter");
    if (!options_.preferences_file)
        throw std::runtime_error("registered kit check: needs --preferences-file");
    preference_values_.clear();
    oa::platform::preferences::save(preference_path_, preference_values_);
    fake_frontend_tick_ = 1000U;

    const auto window_of = [this](int width, int height) {
        require(
            SDL_SetWindowSize(sdl_.window, width, height) && SDL_SyncWindow(sdl_.window),
            "the window did not take " + std::to_string(width) + 'x' + std::to_string(height)
        );
        apply_output_mode();
    };
    window_of(kCanvasWidth, kCanvasHeight);
    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open");
    require(engine_settings_fonts() != nullptr, "the dialog's fonts did not load");

    Applied applied{};
    SettingsPageDesc page{};
    page.id = "check.page";
    page.title = "Check";
    page.owner = "Check";
    page.rows = page_rows();
    page.row_count = kPageRows;
    page.applied = on_applied;
    page.state = &applied;
    require(
        settings_page_register(&oa_layer().registered_settings(), page) == 1,
        "the page was not registered"
    );

    ScreenModel model{};
    KitScreenDesc screen{};
    screen.name = "check.screen";
    screen.model = &model;
    screen.lay_out = lay_screen;
    screen.act = on_act;
    screen.closed = on_closed;
    require(kit_screen_register(&screens_, screen), "the screen was not registered");

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
    const auto named = [this](std::string_view name) -> std::optional<AutomationControl> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls) {
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        }
        return std::nullopt;
    };
    const auto click_named = [&](std::string_view name) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), std::string(name) + " is not shown");
        const layout::Point pixel{
            control->window_x + control->window_width / 2,
            control->window_y + control->window_height / 2
        };
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
            require(running, "a click ended the run");
        }
    };
    const auto open_settings = [&] {
        tap(SDLK_COMMA, kShortcutModifier);
        require(
            engine_settings_dialog() != nullptr && oa_layer().find("settings") != nullptr,
            "the shortcut did not open Settings"
        );
    };
    // The section list scrolls once it is longer than the dialog, as it is in
    // a small window: the wheel over it brings Check's entry into view, as a
    // player would.
    const auto check_entry_laid_out = [&] {
        const settings::Dialog* dialog = engine_settings_dialog();
        if (dialog == nullptr)
            return false;
        const int32_t entry = settings::registered_nav_control(0);
        for (const settings::LayoutPart& part : settings::dialog_layout(*dialog)) {
            if (part.control == entry)
                return true;
        }
        return false;
    };
    const auto show_check_entry = [&] {
        for (int notch = 0; notch < 24 && !check_entry_laid_out(); ++notch) {
            std::vector<AutomationControl> controls;
            oa_layer().automation_controls(controls);
            const AutomationControl* over = nullptr;
            for (const AutomationControl& control : controls) {
                if (control.name.starts_with("oa.settings.nav.") && control.visible &&
                    control.window_width > 0 && control.window_height > 0) {
                    over = &control;
                    break;
                }
            }
            require(over != nullptr, "no entry of the section list is shown");
            const layout::Point pixel{
                over->window_x + over->window_width / 2, over->window_y + over->window_height / 2
            };
            SDL_Event motion =
                window_pointer_event(sdl_.window, sdl_.renderer, SDL_EVENT_MOUSE_MOTION, pixel, 0);
            bool running = true;
            dispatch_event(motion, running);
            SDL_Event wheel{};
            wheel.type = SDL_EVENT_MOUSE_WHEEL;
            wheel.wheel.windowID = motion.motion.windowID;
            wheel.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
            wheel.wheel.y = -1.0F;
            wheel.wheel.mouse_x = motion.motion.x;
            wheel.wheel.mouse_y = motion.motion.y;
            dispatch_event(wheel, running);
            require(running, "the wheel ended the run");
        }
        require(
            check_entry_laid_out(),
            "the wheel over the section list did not bring Check's entry into view"
        );
    };
    const auto page_under_check = [&] {
        const settings::Dialog* dialog = engine_settings_dialog();
        require(dialog != nullptr, "Settings closed before the page was read");
        const auto parts = settings::dialog_layout(*dialog);
        int32_t developer_y = -1;
        int32_t divider_y = -1;
        int32_t divider_x = 0x7fffffff;
        int32_t entry_y = -1;
        const int32_t developer = settings::page_control(settings::Page::developer);
        const int32_t entry = settings::registered_nav_control(0);
        for (const settings::LayoutPart& part : parts) {
            if (part.control == developer && part.text == "Developer")
                developer_y = part.rect.y;
            if (part.control == settings::no_control && part.text == "Check" &&
                part.rect.x < divider_x) {
                divider_x = part.rect.x;
                divider_y = part.rect.y;
            }
            if (part.control == entry && part.text == "Check")
                entry_y = part.rect.y;
        }
        require(
            developer_y >= 0 && divider_y >= 0 && entry_y >= 0 && developer_y < divider_y &&
                divider_y < entry_y,
            "the page is not under a divider reading Check after Developer (Developer at " +
                std::to_string(developer_y) + ", the divider at " + std::to_string(divider_y) +
                ", the entry at " + std::to_string(entry_y) + ")"
        );
    };

    const std::array<WindowCase, 3> windows{{
        {kCanvasWidth, kCanvasHeight, kit::SizeClass::compact, 1, true},
        {larger_layer_windows[0].width,
         larger_layer_windows[0].height,
         larger_layer_windows[0].size_class,
         larger_layer_windows[0].scale,
         false},
        {larger_layer_windows[1].width,
         larger_layer_windows[1].height,
         larger_layer_windows[1].size_class,
         larger_layer_windows[1].scale,
         false},
    }};
    for (const WindowCase& window : windows) {
        const std::string on = std::to_string(window.width) + 'x' + std::to_string(window.height);
        window_of(window.width, window.height);
        open_settings();
        show_check_entry();
        require(
            oa_layer().screen_class() == window.size_class &&
                oa_layer().screen_scale() == window.scale,
            "the page is not at " + std::string(size_class_name(window.size_class)) + ", " +
                std::to_string(window.scale) + "x, on the " + on + " window"
        );
        page_under_check();
        std::cout << "registered kit check: the page at " << size_class_name(window.size_class)
                  << ", " << window.scale << "x, on the " << on << " window\n";
        if (window.write) {
            click_named("oa.settings.nav.check.page");
            click_named("oa.settings.check.page.toggle.off");
            click_named("oa.settings.ok");
            require(engine_settings_dialog() == nullptr, "OK did not close Settings");
            const auto stored = preference_values_.find("check.toggle");
            require(
                stored != preference_values_.end() && stored->second == "0",
                "OK did not store check.toggle as 0"
            );
            require(
                applied.calls == 1 && applied.key == "check.toggle" && applied.value == "0",
                "applied was not called once with check.toggle"
            );
        } else {
            click_named("oa.settings.cancel");
            require(engine_settings_dialog() == nullptr, "Cancel did not close Settings");
            require(applied.calls == 1, "Cancel called applied");
        }
    }

    open_settings();
    show_check_entry();
    click_named("oa.settings.nav.check.page");
    int32_t presses = 0;
    while (engine_settings_dialog() != nullptr &&
           engine_settings_dialog()->focused != settings::first_row_control + 1 && presses < 40) {
        tap(SDLK_TAB);
        ++presses;
    }
    require(
        engine_settings_dialog() != nullptr &&
            engine_settings_dialog()->focused == settings::first_row_control + 1,
        "Tab never reached the choice"
    );
    tap(SDLK_RIGHT);
    click_named("oa.settings.cancel");
    require(engine_settings_dialog() == nullptr, "Cancel did not close Settings after a change");
    require(
        !preference_values_.contains("check.mode") && applied.calls == 1 &&
            preference_values_.at("check.toggle") == "0",
        "Cancel wrote a change"
    );
    std::cout << "registered kit check: Cancel wrote nothing\n";

    const LayerScreen* const before = oa_layer().top();
    require(!oa_layer().open_registered_screen("no.such.screen"), "an unknown screen opened");
    require(oa_layer().top() == before, "an unknown name changed the stack");
    for (const WindowCase& window : windows) {
        const std::string on = std::to_string(window.width) + 'x' + std::to_string(window.height);
        window_of(window.width, window.height);
        const int32_t closed_before = model.closed;
        require(oa_layer().open_registered_screen("check.screen"), "the screen did not open");
        const LayerScreen* top = oa_layer().top();
        require(top != nullptr && top->name() == "check.screen", "the screen is not on top");
        require(
            oa_layer().screen_class() == window.size_class &&
                oa_layer().screen_scale() == window.scale,
            "the screen is not at " + std::string(size_class_name(window.size_class)) + " on the " +
                on + " window"
        );
        const std::optional<AutomationControl> close_button = named("oa.check.screen.close");
        require(
            close_button.has_value() && close_button->dialog == "oa.check.screen",
            "the screen's dialog is not oa.check.screen"
        );
        require(top->interaction().focused == kit::no_control, "the screen opened with a focus");
        bool watch_focus = false;
        int32_t edge_x = 0;
        int32_t edge_y = 0;
        std::array<uint8_t, 3> edge_before{};
        const LayerPlacement placed = top->placement(oa_layer().view());
        const auto paint = [&]() {
            renderer::Surface picture;
            picture.width = static_cast<uint32_t>(placed.points_width);
            picture.height = static_cast<uint32_t>(placed.points_height);
            picture.rgb.assign(static_cast<std::size_t>(picture.width) * picture.height * 3U, 0);
            kit::Canvas canvas;
            canvas.surface = &picture;
            canvas.placement = {0, 0, 1, {0, 0, placed.points_width, placed.points_height}};
            oa_layer().top()->draw(canvas);
            return picture;
        };
        const auto sample = [](const renderer::Surface& picture, int32_t x, int32_t y) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * picture.width + static_cast<std::size_t>(x)) * 3U;
            return std::array<uint8_t, 3>{
                picture.rgb[at], picture.rgb[at + 1], picture.rgb[at + 2]
            };
        };
        if (window.write) {
            const kit::DisplayList listed = top->display_list();
            const kit::Control* close = kit::control_of(listed, 4);
            require(close != nullptr, "the screen has no CLOSE button");
            const kit::Rect button = close->rect;
            require(
                placed.points_width > button.x + 2 && placed.points_height > button.y + 2 &&
                    button.x >= kit::compact_metrics.focus_inset,
                "CLOSE is not where its edge can be read"
            );
            const renderer::Surface idle = paint();
            const std::array<uint8_t, 3> face = sample(idle, button.x + 2, button.y + 2);
            edge_x = button.x - kit::compact_metrics.focus_inset;
            edge_y = button.y + button.height / 2;
            edge_before = sample(idle, edge_x, edge_y);
            const layout::Point pixel{
                close_button->window_x + close_button->window_width / 2,
                close_button->window_y + close_button->window_height / 2
            };
            SDL_Event motion =
                window_pointer_event(sdl_.window, sdl_.renderer, SDL_EVENT_MOUSE_MOTION, pixel, 0);
            bool running = true;
            dispatch_event(motion, running);
            require(running, "a pointer move ended the run");
            top = oa_layer().top();
            require(
                top != nullptr && top->interaction().hovered == 4, "the pointer is not over CLOSE"
            );
            const std::array<uint8_t, 3> lit = sample(paint(), button.x + 2, button.y + 2);
            require(lit != face, "hovering CLOSE did not change its pixels");
            std::cout << "registered kit check: hovering the button changes its pixels\n";
            watch_focus = true;
        }
        const std::array<kit::ControlId, 4> order{1, 2, 3, 4};
        for (const kit::ControlId id : order) {
            tap(SDLK_TAB);
            top = oa_layer().top();
            require(
                top != nullptr && top->name() == "check.screen" && top->interaction().focused == id,
                "Tab did not follow the screen's controls on the " + on + " window"
            );
            if (watch_focus && id == 4) {
                require(
                    sample(paint(), edge_x, edge_y) != edge_before,
                    "Tab onto CLOSE did not draw the focus ring"
                );
                std::cout << "registered kit check: Tab draws the focus ring\n";
            }
        }
        tap(SDLK_RETURN);
        require(oa_layer().find("check.screen") == nullptr, "CLOSE did not pop the screen");
        require(model.closed == closed_before + 1, "closed was not called when the screen closed");
        std::cout << "registered kit check: the screen at " << size_class_name(window.size_class)
                  << ", " << window.scale << "x, on the " << on << " window\n";
    }

    window_of(kCanvasWidth, kCanvasHeight);
    require(screen_ == Screen::main_menu, "the main menu was left");
    const char* notice_lines[] = {"The registered notice."};
    NoticeRequest notice{};
    notice.title = "Registered";
    notice.paragraphs = notice_lines;
    notice.paragraph_count = 1;
    const uint32_t notice_id = oa_layer().queue_notice(notice);
    require(notice_id != 0, "the notice was refused");
    const LayerScreen* registered_notice = oa_layer().find("registered");
    require(
        registered_notice != nullptr &&
            registered_notice->placement(oa_layer().view()).shown.width > 0,
        "the registered notice did not show"
    );

    Answers answered{};
    const char* prompt_lines[] = {"Choose."};
    const char* prompt_buttons[] = {"No", "Yes"};
    const char* prompt_ids[] = {"no", "yes"};
    PromptRequest prompt{};
    prompt.title = "Registered prompt";
    prompt.paragraphs = prompt_lines;
    prompt.paragraph_count = 1;
    prompt.buttons = prompt_buttons;
    prompt.button_count = 2;
    prompt.button_ids = prompt_ids;
    prompt.cancel_button = 0;
    prompt.primary_button = 1;
    prompt.answered = on_answered;
    prompt.user = &answered;
    const uint32_t prompt_id = oa_layer().queue_prompt(prompt);
    require(prompt_id != 0 && prompt_id != notice_id, "the prompt was refused");
    require(
        oa_layer().find("registered") != nullptr && oa_layer().find("prompt") == nullptr &&
            oa_layer().waiting("prompt") != nullptr,
        "the prompt did not wait behind the notice"
    );

    preference_values_[std::string(saves_moved_preference)] = "1 0";
    preference_values_[std::string(saves_notice_preference)] = std::string(saves_notice_due);
    user_folder_state().check_shows_notice = true;
    for (int pass = 0; pass < 4; ++pass)
        tell_saves_moved();
    require(
        oa_layer().find("registered") != nullptr && oa_layer().find("prompt") == nullptr &&
            oa_layer().find("notice") == nullptr && oa_layer().waiting("prompt") != nullptr &&
            oa_layer().waiting("notice") != nullptr,
        "the notice, the prompt and the saves notice were not queued in order"
    );
    std::cout
        << "registered kit check: the notice shows, and the prompt and the saves notice wait\n";

    click_named("oa.registered.ok");
    require(
        oa_layer().find("registered") == nullptr && oa_layer().find("prompt") != nullptr &&
            oa_layer().find("notice") == nullptr && oa_layer().waiting("notice") != nullptr,
        "the prompt did not show alone once the notice closed"
    );
    std::cout << "registered kit check: the prompt shows, and the saves notice waits\n";

    click_named("oa.prompt.yes");
    require(
        answered.calls == 1 && answered.button == 1 && answered.id == prompt_id,
        "Yes did not answer the prompt once with 1"
    );
    require(
        oa_layer().find("prompt") == nullptr && oa_layer().find("notice") != nullptr,
        "the saves notice did not show once the prompt closed"
    );
    std::cout << "registered kit check: the saves notice shows\n";

    Answers dropped{};
    prompt.user = &dropped;
    const uint32_t dropped_id = oa_layer().queue_prompt(prompt);
    require(dropped_id != 0 && dropped_id != prompt_id, "the second prompt was refused");
    require(
        oa_layer().waiting("prompt") != nullptr && oa_layer().find("prompt") == nullptr &&
            dropped.calls == 0,
        "the second prompt did not wait behind the saves notice"
    );
    oa_layer().drop_pending_prompts();
    oa_layer().drop_pending_prompts();
    require(
        dropped.calls == 1 && dropped.button == -1 && dropped.id == dropped_id,
        "dropping the queue did not answer the prompt once with -1"
    );
    require(answered.calls == 1, "the first prompt was answered again");
    require(
        oa_layer().find("notice") != nullptr && oa_layer().find("prompt") == nullptr &&
            oa_layer().waiting("prompt") == nullptr,
        "dropping the queue took the saves notice"
    );
    std::cout << "registered kit check: the dropped prompt answered -1 once\n";
    std::cout << "registered kit check: passed\n";
}

} // namespace oa::app
