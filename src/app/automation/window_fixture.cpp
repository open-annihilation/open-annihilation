// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A window the automation check drives. With --fixture-window the main menu
// carries one button and a label of the idle clock, which a program driving
// the game lists and clicks. Without the option the extension does nothing.
#include "oa/app/app.hpp"
#include "oa/app/extension.hpp"
#include "oa/ui/screen_registry.hpp"

#include <cstdio>
#include <string>
#include <string_view>

// The version of the engine's extension table these hooks follow. A build
// against a table of another version stops here until the engine's change
// has been read and the hooks follow it. Version 14 adds functions an
// extension calls rather than hooks (extension.hpp lists them).
// This extension calls set_extension_window_source and extension_clock.
// Version 15 adds the OA UI kit, which this extension does not use.
// Version 16 adds run_mutators, which this extension does not call.
// Version 17 adds hear_remote_seat_chat, which this extension does not call.
// Version 18 adds the kit's key pictures, which this extension does not use.
constexpr uint32_t kExtensionApiVersionFollowed = 18;
static_assert(
    oa::app::extension_api_version == kExtensionApiVersionFollowed,
    "the engine's extension table changed: follow its change, then raise "
    "kExtensionApiVersionFollowed"
);

namespace oa::app {
namespace {

// The button and the clock label, on the main menu's canvas, clear of its
// own buttons.
constexpr int32_t kButtonX = 560;
constexpr int32_t kButtonY = 444;
constexpr int32_t kButtonWidth = 72;
constexpr int32_t kButtonHeight = 24;
constexpr int32_t kClockX = 560;
constexpr int32_t kClockY = 416;
constexpr int32_t kClockWidth = 72;
constexpr int32_t kClockHeight = 20;
// Above the notices drawn over the main menu, so a click on the button
// reaches this window.
constexpr int16_t kOverlayZ = 200;
constexpr const char* kOption = "--fixture-window";
constexpr const char* kWindowName = "fixture";

// Whether the option is on, and whether the button has been clicked.
struct Fixture {
    bool enabled{};
    bool pressed{};
};

/// Returns the process's fixture.
///
/// @return the one fixture, made on first use
Fixture& fixture_state() {
    static Fixture state;
    return state;
}

/// Tells whether a point on the canvas lies on the button.
///
/// @param x its column
/// @param y its row
/// @return true when it lies on the button
bool inside_button(float x, float y) noexcept {
    return x >= static_cast<float>(kButtonX) && x < static_cast<float>(kButtonX + kButtonWidth) &&
           y >= static_cast<float>(kButtonY) && y < static_cast<float>(kButtonY + kButtonHeight);
}

/// Takes --fixture-window (Extension::take_option).
///
/// @param context the Fixture the option sets
/// @param name the option with its dashes
/// @return false when the option is not this one
bool take_option(void* context, const char* name, const OptionValues&, uint32_t&) {
    if (name == nullptr || std::string_view(name) != kOption)
        return false;
    static_cast<Fixture*>(context)->enabled = true;
    return true;
}

/// Returns the option's usage text (Extension::text).
///
/// @param which the text asked for
/// @return the option for the run options' line and the line after the
///         usage; null for the others
const char* text(void*, ExtensionText which) {
    if (which == ExtensionText::usage_runs)
        return "[--fixture-window] ";
    if (which == ExtensionText::usage_note)
        return "--fixture-window shows a window a program driving the game can list and click.\n";
    return nullptr;
}

/// Lists the fixture's window (ExtensionWindowSource).
///
/// @param context the Fixture
/// @param runtime the running app, whose clock the label reads
/// @param[out] windows receives the window while the option is on
void list_windows(void* context, const Runtime& runtime, std::vector<ExtensionWindow>& windows) {
    const auto& state = *static_cast<const Fixture*>(context);
    if (!state.enabled)
        return;
    ExtensionWindow window;
    window.name = kWindowName;
    ExtensionControl button;
    button.name = "press";
    button.kind = ExtensionControlKind::button;
    button.x = kButtonX;
    button.y = kButtonY;
    button.width = kButtonWidth;
    button.height = kButtonHeight;
    button.text = state.pressed ? "pressed" : "ready";
    ExtensionControl clock;
    clock.name = "clock";
    clock.kind = ExtensionControlKind::label;
    clock.x = kClockX;
    clock.y = kClockY;
    clock.width = kClockWidth;
    clock.height = kClockHeight;
    clock.enabled = false;
    char digits[16];
    std::snprintf(digits, sizeof digits, "%u", extension_clock(runtime));
    clock.text = digits;
    window.controls.push_back(std::move(button));
    window.controls.push_back(std::move(clock));
    windows.push_back(std::move(window));
}

/// Takes a click on the button (OverlayDesc::event).
///
/// @param ctx the screen context, whose input is the event
/// @param state the Fixture
/// @return 1 when the event is a press or a release on the button
int on_event(ScreenContext* ctx, void* state) {
    auto* fixture = static_cast<Fixture*>(state);
    if (fixture == nullptr || !fixture->enabled || ctx == nullptr || ctx->input == nullptr)
        return 0;
    const ScreenInput& input = *ctx->input;
    if (input.button != 1 ||
        (input.kind != ScreenInputKind::pointer_down && input.kind != ScreenInputKind::pointer_up))
        return 0;
    if (!inside_button(input.x, input.y))
        return 0;
    if (input.kind == ScreenInputKind::pointer_down)
        fixture->pressed = true;
    return 1;
}

/// Shows the window once the runtime exists (Extension::startup).
///
/// @param context the Fixture
/// @param runtime the runtime being built
void startup(void* context, Runtime& runtime) {
    auto* state = static_cast<Fixture*>(context);
    if (state->enabled)
        set_extension_window_source(runtime, state, list_windows);
}

/// Registers the overlay that takes the button's click (Extension::register_screens).
///
/// @param context the Fixture
/// @param[in,out] registry the runtime's registry
void register_screens(void* context, ScreenRegistry* registry) {
    auto* state = static_cast<Fixture*>(context);
    if (!state->enabled || registry == nullptr)
        return;
    OverlayDesc overlay{};
    overlay.name = "fixture window";
    overlay.screen = screen_id(Screen::main_menu);
    overlay.z = kOverlayZ;
    overlay.event = on_event;
    overlay.state = state;
    overlay_register(registry, &overlay);
}

} // namespace
} // namespace oa::app

/// Fills the fixture window's extension table.
///
/// @param[in,out] table the table, zeroed
void oa_extension_init_window_fixture(oa::app::Extension* table) {
    auto& state = oa::app::fixture_state();
    table->context = &state;
    table->take_option = oa::app::take_option;
    table->text = oa::app::text;
    table->startup = oa::app::startup;
    table->register_screens = oa::app::register_screens;
}
