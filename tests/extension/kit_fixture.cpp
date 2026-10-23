// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Kit fixture: a test extension that uses the OA UI kit through the
// extension table alone, as version 15 of the table offers it.
//
// - Its register_settings_pages registers a Settings page, Kit fixture, under
//   a divider of the same name: a toggle bound to kit-fixture.show-chip (On
//   by default) and a choice bound to kit-fixture.mode (quiet by default).
//   When OK stores a change it prints "kit fixture: applied <key>=<value>".
// - Its register_screens registers a kit screen, kit-fixture.screen: a
//   section's heading, three list rows and CLOSE, in that tab order. When it
//   closes it prints "kit fixture: screen closed".
// - With --kit-fixture-screen it opens that screen on the main menu's first
//   frame (ScreenServices::open_kit_screen); with --kit-fixture-prompt it
//   asks "Fixture question" there, with No and Yes (ask_prompt), and prints
//   "kit fixture: answered <id>" with the button's id.
// - Over a match's battlefield, while kit-fixture.show-chip holds 1, it draws
//   a chip, a progress bar half full and a line of kit text, from the
//   battlefield's top left, 8 pixels in at the overlay's scale, and prints
//   once "kit fixture: chip at <x>,<y> <width>x<height> outline <rrggbb>": the
//   chip's rectangle in the overlay's pixels and the outline the kit's theme
//   gives it.
//
// tests/extension/check_kit_fixture.py (extension-kit) drives it. It is a
// test extension, so only a build configured with
// -DOA_RECORD_EXTENSION_HOOKS=ON combines it.
#include "oa/app/app.hpp"
#include "oa/app/extension.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/rows.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/screen_registry.hpp"
#include "oa/ui/screen_registry/kit.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

// The version of the engine's extension table this extension follows. A
// build against a table of another version stops here until the engine's
// change has been read and the extension follows it. Version 15 adds the OA
// UI kit (extension.hpp): this extension uses register_settings_pages, a kit
// screen, open_kit_screen, ask_prompt and the overlay's kit calls.
// Version 16 adds run_mutators, which this extension does not call.
// Version 17 adds hear_remote_seat_chat, which this extension does not call.
constexpr uint32_t kExtensionApiVersionFollowed = 17;
static_assert(
    oa::app::extension_api_version == kExtensionApiVersionFollowed,
    "the engine's extension table changed: follow its change, then raise "
    "kExtensionApiVersionFollowed"
);

namespace kit_fixture {
namespace {

namespace app = oa::app;
namespace kit = oa::ui::kit;

// The launch options.
constexpr std::string_view kScreenOption = "--kit-fixture-screen";
constexpr std::string_view kPromptOption = "--kit-fixture-prompt";

// The Settings page and the preference keys its rows are bound to.
constexpr const char* kPageId = "kit-fixture.options";
constexpr const char* kPageTitle = "Kit fixture";
constexpr const char* kPageOwner = "Kit fixture";
constexpr std::string_view kShowChipKey = "kit-fixture.show-chip";
constexpr std::string_view kModeKey = "kit-fixture.mode";
// What show-chip holds while the chip is drawn, and what it reads when none
// is stored.
constexpr std::string_view kShowChipOn = "1";

// The kit screen.
constexpr const char* kScreenName = "kit-fixture.screen";
// Its controls, in their tab order.
constexpr kit::ControlId kFirstRow = 1;
constexpr kit::ControlId kRowCount = 3;
constexpr kit::ControlId kClose = kFirstRow + kRowCount;
// The columns and rows between the screen's edges and its parts, and between
// its parts, in points.
constexpr int32_t kScreenPadding = 12;
constexpr int32_t kScreenGap = 8;
constexpr int32_t kRowGap = 2;
// CLOSE's width, in points.
constexpr int32_t kCloseWidth = 80;
// What open_kit_screen answers when it opened the screen.
constexpr int kOpened = 1;
// What a kit screen's act answers: nothing, or close.
constexpr int kActNothing = 0;
constexpr int kActClose = 2;

// Above the OA layer, so that the main menu's first frame reaches the
// overlay's tick whatever the layer shows.
constexpr int16_t kOverlayZ = 210;

// The overlay's parts: 8 pixels of the 640x480 screen in from the
// battlefield's top left, 4 between them, and the bar's size, each at the
// overlay's scale.
constexpr int kOverlayInset = 8;
constexpr int kOverlayGap = 4;
constexpr int kBarWidth = 64;
constexpr int kBarHeight = 6;
// The bar's progress: half, in thousandths.
constexpr int32_t kBarHalf = 500;
// The chip's text and the text line's.
constexpr const char* kChipText = "Fixture";
constexpr const char* kLineText = "Kit fixture";

// What the extension keeps for the process.
struct Fixture {
    bool open_screen{};                    ///< --kit-fixture-screen was given
    bool ask_prompt{};                     ///< --kit-fixture-prompt was given
    bool asked{};                          ///< the screen or the prompt was asked for
    void* host{};                          ///< ScreenContext::host, kept from the overlay
    const app::ScreenServices* services{}; ///< ScreenContext::services, kept likewise
    bool chip_printed{};                   ///< the chip's line was printed
};

/// Returns the process's fixture.
///
/// @return the one fixture, made on first use
Fixture& fixture_state() {
    static Fixture state;
    return state;
}

/// Prints a line of the fixture's and sends it at once, so that a check
/// reading the game's output sees it while the game runs.
///
/// @param line the line, without its line break
void say(const std::string& line) {
    std::printf("kit fixture: %s\n", line.c_str());
    std::fflush(stdout);
}

/// Takes --kit-fixture-screen and --kit-fixture-prompt (Extension::take_option).
///
/// @param context the Fixture the option sets
/// @param name the option with its dashes
/// @return false when the option is neither
bool take_option(void* context, const char* name, const app::OptionValues&, uint32_t&) {
    if (name == nullptr)
        return false;
    auto& state = *static_cast<Fixture*>(context);
    if (name == kScreenOption) {
        state.open_screen = true;
        return true;
    }
    if (name == kPromptOption) {
        state.ask_prompt = true;
        return true;
    }
    return false;
}

/// Returns the options' usage text (Extension::text).
///
/// @param which the text asked for
/// @return the options for the run options' line; null for the others
const char* text(void*, app::ExtensionText which) {
    if (which == app::ExtensionText::usage_runs)
        return "[--kit-fixture-screen] [--kit-fixture-prompt] ";
    return nullptr;
}

/// Prints a key OK stored (SettingsPageDesc::applied).
///
/// @param key the key, read at once
/// @param value the value stored, read at once
void page_applied(void*, const char* key, const char* value) {
    say(std::string("applied ") + (key != nullptr ? key : "") + '=' +
        (value != nullptr ? value : ""));
}

/// Returns the Settings page's rows.
///
/// @return the two rows, which live until the process exits
const kit::PreferenceRow* page_rows() {
    static const std::array<std::string_view, 2> mode_ids{"quiet", "loud"};
    static const std::array<std::string_view, 2> mode_captions{"Quiet", "Loud"};
    static const kit::PreferenceRow rows[] = {
        kit::preference_toggle(
            "show-chip",
            "Show the fixture's chip",
            kShowChipKey,
            kShowChipOn,
            {"A chip, a bar and a line of text over the battlefield."}
        ),
        kit::preference_choice("mode", "Fixture mode", kModeKey, "quiet", mode_ids, mode_captions),
    };
    return rows;
}

/// Registers the Settings page (Extension::register_settings_pages).
///
/// @param context the Fixture
/// @param[in,out] registry the runtime's Settings pages
void register_settings_pages(void* context, app::SettingsRegistry* registry) {
    app::SettingsPageDesc page{};
    page.id = kPageId;
    page.title = kPageTitle;
    page.owner = kPageOwner;
    page.rows = page_rows();
    page.row_count = 2;
    page.applied = page_applied;
    page.state = context;
    // A page the registry refuses stops the start once this returns.
    (void)app::settings_page_register(registry, page);
}

/// Lays the kit screen out in its frame: the section's heading, three list
/// rows and CLOSE, one under another, at the frame's size class.
///
/// @param frame the frame the OA layer gives the screen
/// @return the display list
kit::DisplayList lay_out_screen(void*, const kit::Frame& frame) {
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    const int32_t width = std::max(frame.area.width, 2 * kScreenPadding + kCloseWidth);
    const int32_t height = std::max(frame.area.height, 1);
    const int32_t inner = width - 2 * kScreenPadding;
    kit::DisplayList list;
    kit::Item face;
    face.role = kit::Role::fill;
    face.rect = {0, 0, width, height};
    face.colour = kit::colour::panel;
    list.items.push_back(std::move(face));
    kit::Item heading;
    heading.role = kit::Role::heading;
    heading.rect = {kScreenPadding, kScreenPadding, inner, metrics.regular_line};
    heading.look = kit::HeadingLook{"KIT FIXTURE"};
    list.items.push_back(std::move(heading));
    static const std::array<const char*, kRowCount> titles{"First row", "Second row", "Third row"};
    static const std::array<const char*, kRowCount> names{
        "kit-fixture.screen.first", "kit-fixture.screen.second", "kit-fixture.screen.third"
    };
    int32_t top = kScreenPadding + metrics.regular_line + kScreenGap;
    for (kit::ControlId row = 0; row < kRowCount; ++row) {
        const auto at = static_cast<std::size_t>(row);
        kit::ListRowLook look;
        look.title = titles[at];
        look.lines = {"A row of the fixture's screen."};
        kit::add_list_row(
            list,
            {kScreenPadding, top, inner, metrics.list_row_height},
            look,
            kFirstRow + row,
            names[at]
        );
        top += metrics.list_row_height + kRowGap;
    }
    top += kScreenGap - kRowGap;
    kit::ButtonLook close_look;
    close_look.caption = "CLOSE";
    close_look.style = kit::ButtonStyle::accent;
    kit::add_button(
        list,
        {kScreenPadding, top, kCloseWidth, metrics.button_height},
        close_look,
        kClose,
        "kit-fixture.screen.close"
    );
    list.tab_order = {kFirstRow, kFirstRow + 1, kFirstRow + 2, kClose};
    return list;
}

/// Closes the kit screen on CLOSE or Escape (KitScreenDesc::act).
///
/// @param action the press, or the key no control took
/// @return kActClose for CLOSE or Escape; kActNothing otherwise
int act_screen(void*, const app::KitScreenAction& action) {
    if (action.control == kClose)
        return kActClose;
    if (action.control == kit::no_control && action.key == kit::Key::escape)
        return kActClose;
    return kActNothing;
}

/// Prints that the kit screen closed (KitScreenDesc::closed).
void screen_closed(void*) {
    say("screen closed");
}

/// Prints the prompt's answer (PromptRequest::answered).
///
/// @param button the button pressed, from 0, or -1 when the run dropped the prompt
void prompt_answered(void*, uint32_t, int32_t button) {
    static constexpr std::array<const char*, 2> ids{"no", "yes"};
    const bool known = button >= 0 && static_cast<std::size_t>(button) < ids.size();
    say(std::string("answered ") + (known ? ids[static_cast<std::size_t>(button)] : "none"));
}

/// Asks the fixture's prompt through the services: "Fixture question", No
/// and Yes, Escape answering No and Yes the one the prompt leads to.
///
/// @param state the fixture, its services kept
void ask(const Fixture& state) {
    const char* const paragraphs[] = {"Does the fixture's prompt reach its answer?"};
    const char* const buttons[] = {"No", "Yes"};
    const char* const button_ids[] = {"no", "yes"};
    app::PromptRequest request{};
    request.title = "Fixture question";
    request.paragraphs = paragraphs;
    request.paragraph_count = 1;
    request.buttons = buttons;
    request.button_count = 2;
    request.button_ids = button_ids;
    request.cancel_button = 0;
    request.primary_button = 1;
    request.answered = prompt_answered;
    // The request is copied at once, so these arrays may go when it returns.
    if (state.services->ask_prompt(state.host, &request) == 0)
        say("the prompt was refused");
}

/// Keeps the host and services the overlay is created with (OverlayDesc::create).
///
/// @param ctx the overlay's context
/// @param state the Fixture
void keep_services(app::ScreenContext* ctx, void* state) {
    auto& kept = *static_cast<Fixture*>(state);
    kept.host = ctx->host;
    kept.services = ctx->services;
}

/// Opens the kit screen or asks the prompt on the main menu's first frame,
/// as the launch options say (OverlayDesc::tick).
///
/// @param state the Fixture
void first_main_menu_frame(app::ScreenContext*, void* state) {
    auto& kept = *static_cast<Fixture*>(state);
    if (kept.asked || kept.services == nullptr || (!kept.open_screen && !kept.ask_prompt))
        return;
    kept.asked = true;
    if (kept.open_screen && kept.services->open_kit_screen(kept.host, kScreenName) != kOpened)
        say("the screen did not open");
    if (kept.ask_prompt)
        ask(kept);
}

/// Registers the kit screen and the overlay that keeps the services and
/// acts on the main menu's first frame (Extension::register_screens).
///
/// @param context the Fixture
/// @param[in,out] registry the runtime's registry
void register_screens(void* context, app::ScreenRegistry* registry) {
    app::KitScreenDesc screen{};
    screen.name = kScreenName;
    screen.lay_out = lay_out_screen;
    screen.act = act_screen;
    screen.closed = screen_closed;
    // A screen the registry refuses stops the start once this returns.
    (void)app::kit_screen_register(registry, screen);
    app::OverlayDesc overlay{};
    overlay.name = "kit fixture";
    overlay.screen = app::screen_id(app::Screen::main_menu);
    overlay.z = kOverlayZ;
    overlay.create = keep_services;
    overlay.tick = first_main_menu_frame;
    overlay.state = context;
    (void)app::overlay_register(registry, &overlay);
}

/// Tells whether kit-fixture.show-chip holds 1, as the preferences hold it,
/// or as the row reads it when none is stored.
///
/// @param state the fixture, its services kept
/// @return true while the chip is shown
bool chip_shown(const Fixture& state) {
    if (state.services == nullptr)
        return true;
    char stored[8]{};
    const std::string key(kShowChipKey);
    if (state.services->read_string(state.host, nullptr, key.c_str(), stored, sizeof stored) == 0)
        return true;
    return std::string_view(stored) == kShowChipOn;
}

/// Returns a colour as six lower-case hexadecimal digits, red first.
///
/// @param ink the colour
/// @return "rrggbb"
std::string hex_colour(kit::Colour ink) {
    char digits[8]{};
    std::snprintf(digits, sizeof digits, "%02x%02x%02x", ink.r, ink.g, ink.b);
    return digits;
}

/// Draws the chip, the bar and the text line over the battlefield while the
/// chip is shown, and prints the chip's place once
/// (Extension::draw_match_overlay).
///
/// @param context the Fixture
/// @param overlay the battlefield and its painter
void draw_match_overlay(void* context, app::Runtime&, const app::MatchOverlay& overlay) {
    auto& state = *static_cast<Fixture*>(context);
    if (!chip_shown(state) || overlay.kit_text_width == nullptr || overlay.draw_chip == nullptr ||
        overlay.draw_progress == nullptr || overlay.draw_kit_text == nullptr)
        return;
    const int scale = std::max(overlay.scale, 1);
    const int left = overlay.left + kOverlayInset * scale;
    int top = overlay.top + kOverlayInset * scale;
    kit::ChipLook chip;
    chip.text = kChipText;
    chip.state = kit::ChipState::installed;
    // A chip is its text in the small font with the kit's padding either
    // side, and the kit's chip height (controls.hpp, theme.hpp).
    const int chip_width =
        overlay.kit_text_width(overlay.painter, kit::FontRole::small, kChipText) +
        2 * kit::compact_metrics.chip_padding * scale;
    const int chip_height = kit::compact_metrics.chip_height * scale;
    overlay.draw_chip(overlay.painter, left, top, &chip);
    if (!state.chip_printed) {
        state.chip_printed = true;
        // An installed chip's outline is the accent (draw_chip).
        say("chip at " + std::to_string(left) + ',' + std::to_string(top) + ' ' +
            std::to_string(chip_width) + 'x' + std::to_string(chip_height) + " outline " +
            hex_colour(kit::colour::accent));
    }
    top += chip_height + kOverlayGap * scale;
    overlay.draw_progress(
        overlay.painter, left, top, kBarWidth * scale, kBarHeight * scale, kBarHalf
    );
    top += (kBarHeight + kOverlayGap) * scale;
    overlay.draw_kit_text(overlay.painter, kit::FontRole::regular, left, top, kLineText);
}

} // namespace
} // namespace kit_fixture

/// Fills the Kit fixture's extension table (its oa_add_extension INIT).
///
/// @param[out] table the table, zeroed on entry
void oa_extension_init_kit_fixture(oa::app::Extension* table) {
    auto& state = kit_fixture::fixture_state();
    table->context = &state;
    table->take_option = kit_fixture::take_option;
    table->text = kit_fixture::text;
    table->register_screens = kit_fixture::register_screens;
    table->register_settings_pages = kit_fixture::register_settings_pages;
    table->draw_match_overlay = kit_fixture::draw_match_overlay;
}
