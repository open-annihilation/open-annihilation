// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards' extension, filled by oa_extension_init_boneyards and built
// on network play's (oa/app/netgame/extension_api.hpp): the online service's start
// (Rover's session, a replay standing in for it, and the pages' text
// service), the main-menu strip with its screens, steps and the frontend's
// questions about a launched battle, the per-frame idle work, the "Exit to
// Windows?" box, the Boneyards checks, which drive the running game through
// the engine's check host, the game's files read through the engine's
// read_game_file and, when those files do not hold the path, the archive
// that travels with the program, and the answers after a Boneyards launch to the
// engine's questions (the label the match's return names, the stored
// password) and to network play's hooks (the match report, the console's
// Page, a failed launched join). The strip's MULTI calls
// answer_multiplayer_entry, so a demo with no multiplayer map still shows
// that notice.
#include "boneyards_data.hpp"
#include "boneyards_exit_prompt.hpp"
#include "boneyards_launch.hpp"
#include "boneyards_launch_check.hpp"
#include "boneyards_menu_check.hpp"
#include "boneyards_options.hpp"
#include "boneyards_report.hpp"
#include "console_state.hpp"
#include "expansion_folder.hpp"
#include "shipped_archive.hpp"

#include "oa/app/app.hpp"
#include "oa/app/check_host.hpp"
#include "oa/app/expansion_entry.hpp"
#include "oa/app/extension.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/netgame/extension_api.hpp"
#include "oa/boneyards/console/menu_entry.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/html_host_fonts.hpp"
#include "oa/boneyards/html_scripts.hpp"
#include "oa/boneyards/rover_replay.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/service.hpp"
#include "oa/boneyards/screens/galaxy.hpp"
#include "oa/boneyards/screens/profile.hpp"
#include "oa/boneyards/screens/registration.hpp"

#include "oa/platform/files.hpp"
#include "oa/platform/fonts.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/platform/system.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The versions of the engine's extension table and of network play's API
// these hooks follow. A build against another version of either stops here
// until its change has been read and the hooks follow it. Version 12 of the
// table adds a remote-controlled option: the main loop keeps running every
// frame while the window is inactive, and the frame hook needs nothing new
// for that. Version 13 adds FrameStage::presented, a third call of the frame
// hook after the frame is shown. Version 14 adds the calls the Boneyards
// makes: player_folder, the player's own folder of a runtime, empty until
// the runtime has chosen it; set_unit_limit, which sets the unit limit
// preference a multiplayer game uses; keep_running_while_inactive, a
// request the extension can hold so the main loop keeps running while the
// window is inactive; web_address_available and open_web_address, which
// open an http or https address in the system's browser where the machine
// has one; the modern text faces and their fallback chain, the player's
// modern-fonts and Text size settings, and set_focused_text_field;
// read_game_file, which reads one file from the game's files;
// set_extension_window_source and extension_clock, through which a program
// driving the game sees an extension's windows and clock; and
// simulation_hash, the hash the run plays under. Version 15 adds the OA UI
// kit to the contract. Version 16 adds run_mutators. Version 17 adds
// hear_remote_seat_chat. Version 18 adds the kit's key pictures. Version 19
// is Kit 2's look in the kit's headers. None of 15 to 19 is a hook this
// module fills.
constexpr uint32_t kExtensionApiVersionFollowed = 20;
static_assert(
    oa::app::extension_api_version == kExtensionApiVersionFollowed,
    "the engine's extension table changed: follow its change, then raise "
    "kExtensionApiVersionFollowed"
);
constexpr uint32_t kNetworkPlayApiVersionFollowed = 3;
static_assert(
    oa::app::netgame::extension_api::api_version == kNetworkPlayApiVersionFollowed,
    "network play's API changed: follow its change, then raise kNetworkPlayApiVersionFollowed"
);

namespace oa::boneyards::console {
void chat_set_modern_text(bool modern_fonts, int32_t text_size);
void boneyards_console_set_multiplayer_answer(bool (*answer)(void* user), void* user);
} // namespace oa::boneyards::console

namespace oa::app {

// The Boneyards overlay's screens, its launch bridge, its text service, its
// idle work and its "Exit to the system?" box (console_screens.cpp).
void register_boneyards_console_screens(ScreenRegistry* registry);
void boneyards_console_set_launch_bridge(const oa::boneyards::console::LaunchBridge& bridge);
void boneyards_console_request_entry_pass();
void boneyards_console_end_on_leave();
void boneyards_console_check_shortcut();
oa::boneyards::console::StatusWidget* boneyards_console_status_widget();
void boneyards_console_set_text_fonts(const oa::boneyards::html::FontService& fonts);
void boneyards_console_set_analogue_atlas_path(const char* path);
void boneyards_console_idle(uint32_t now_ms);
bool boneyards_console_up();
bool boneyards_console_ask_exit();
void boneyards_console_release();
void boneyards_console_defer_main_menu();
void boneyards_console_set_text_field_place(
    void (*place)(void* host, int x, int y, int width, int height, int cursor)
);
void boneyards_console_set_menu_entry(
    const char* mode, int32_t x, int32_t y, const std::filesystem::path& mod_folder
);
bool boneyards_console_menu_entry(
    oa::boneyards::console::MenuEntryMode* mode,
    int32_t* x,
    int32_t* y,
    int32_t* width,
    int32_t* height
);
void boneyards_entry_install(Runtime& runtime);
void boneyards_entry_on_frame(Runtime& runtime, FrameStage stage);

// The --check-* runs. Defined in boneyards_self_checks.cpp, which a release
// build does not compile, so this file includes no header of the checks.
bool run_boneyards_self_checks(Runtime& runtime, RunPhase phase, int& exit_code);
// The inactive-loop check's login. Defined in boneyards_inactive_check.cpp,
// which a release build does not compile.
void run_boneyards_inactive_check(Runtime& runtime);

namespace {

namespace api = oa::app::netgame::extension_api;
namespace mp = oa::ui::frontend_multiplayer;

// The Boneyards box "Exit to the system?", as the game keeps it installed.
ExitPrompt g_exit_prompt;

// The runtime whose player folder a file read or write asks for. The folder
// is empty until the runtime has chosen it, which is after the preferences
// load, so a file touched from startup, register_screens or ready is not
// written into the game folder. Compared by address only once the runtime
// is being destroyed.
Runtime* g_player_folder_runtime = nullptr;

// Whether this extension last wrote the request that the main loop keep
// running while the window is inactive. The request is one latch for the
// whole run, so it is written only when that answer changes. Writing it
// every frame would release a hold another part of the run has taken.
// A remote-controlled run keeps its own hold: this extension does not
// release the latch when its own reason ends.
bool g_background_held = false;

// A login that has been seen live, and one that failed. "-y" keeps the loop
// running from the moment the Boneyards open until the session ends or the
// login fails, and not after. Leaving the Boneyards clears both, before a
// status line left on the dialog can fail the next entry.
struct LoginBackground {
    bool saw_live = false;
    bool failed = false;
    bool was_connecting = false;
};

LoginBackground g_login_background;

/// Tells whether a login's status line is a failure the dialog left showing.
///
/// @param status the dialog's status line
/// @return true for the two lines a failed or cancelled login leaves
bool login_failed_status(const char* status) {
    return std::strcmp(status, "Server Not responding") == 0 ||
           std::strcmp(status, "Transimision aborted by request.") == 0;
}

/// Remembers a login that reached a session, and one that failed.
///
/// Outside the Boneyards, with no login in progress and no session, both are
/// forgotten before the status line is read, so a line left from the last
/// attempt does not fail the next entry.
///
/// @param inside the Boneyards window is open
/// @param live a login has reached a session
/// @param connecting LOGIN is in progress
/// @param status the dialog's status line
void note_login_background(bool inside, bool live, bool connecting, const char* status) {
    if (!inside && !live && !connecting) {
        g_login_background.saw_live = false;
        g_login_background.failed = false;
    } else {
        if (live)
            g_login_background.saw_live = true;
        if (connecting)
            g_login_background.failed = false;
        else if (g_login_background.was_connecting && !live)
            g_login_background.failed = true;
        else if (!live && login_failed_status(status))
            g_login_background.failed = true;
    }
    g_login_background.was_connecting = connecting;
}

/// The inactive-loop check's login. The engine's own check sets the flag
/// with no replay and must be left to count its three frames. This login
/// runs only when a replay is given as well.
///
/// @param runtime the running app
/// @return true when this run should drive the inactive login
bool inactive_login_requested(const Runtime& runtime) {
    return runtime_options(runtime).check_running_while_inactive &&
           !boneyards_options().boneyards_replay.empty();
}

/// Holds the loop while a login or a session needs it, and lets that go.
///
/// The hold starts when LOGIN is pressed, and when "-y" has opened the
/// Boneyards, and lasts until the session ends or the login fails. The
/// inactive-loop check owns the latch for its whole run, so nothing is
/// written then. A remote-controlled run is left holding when this reason
/// ends.
///
/// @param runtime the running app
void hold_background(Runtime& runtime) {
    if (runtime_options(runtime).check_running_while_inactive)
        return;
    const bool live = oa::boneyards::rover_session_live(oa::boneyards::rover_session());
    oa::boneyards::console::StatusWidget* widget = boneyards_console_status_widget();
    const bool inside = oa::boneyards::console::boneyards_status_widget_in_boneyards(widget);
    bool connecting = false;
    const char* status = "";
    if (widget != nullptr && widget->navigation != nullptr) {
        const oa::boneyards::screens::LoginDialog& dialog = widget->navigation->login;
        connecting = dialog.state == oa::boneyards::screens::LoginState::connecting;
        status = dialog.status;
    }
    note_login_background(inside, live, connecting, status);
    const bool opened_by_setup = boneyards_options().setup_requested && inside;
    const bool want =
        live || connecting ||
        (opened_by_setup && !g_login_background.saw_live && !g_login_background.failed);
    if (!want && runtime_options(runtime).remote_controlled) {
        g_background_held = false;
        return;
    }
    if (want == g_background_held)
        return;
    keep_running_while_inactive(runtime, want);
    g_background_held = want;
}

// A headless --load that does not ask for the Boneyards. The strip stays
// unbuilt, and the session and the screens are not set up: the saved game
// never shows them.
bool g_quiet_load = false;

// The lobby the last pump saw, so a change can be posted once. The first
// successful read is remembered and not posted: the login console is often
// already open. A failed read clears it, and the next successful read seeds
// again. console_screen is the console last posted as shown, or none once
// no console is open, so a console that opens behind the green slide is
// posted when it shows.
struct LobbySeen {
    bool seeded{};
    int32_t connection_level{};
    int32_t console_screen{};
    char user_name[oa::boneyards::kSessionNameCapacity]{};
};

LobbySeen g_lobby_seen{};

/// A headless saved-game load that never asks to open the Boneyards.
///
/// The menu is not shown, and no Boneyards option is set, so the session,
/// the catalogues and the screen hooks are not used.
///
/// @param runtime the runtime being built
/// @return true when this run only loads the saved game
bool quiet_saved_game(const Runtime& runtime) {
    const Options& run = runtime_options(runtime);
    if (run.load_file.empty() || !run.headless_check)
        return false;
    const BoneyardsOptions& taken = boneyards_options();
    return taken.online_module.empty() && !taken.setup_requested && !taken.check_boneyards_login &&
           taken.boneyards_replay.empty() && taken.boneyards_server.empty() &&
           !taken.enter_boneyards_ticks && !taken.boneyards_login &&
           taken.boneyards_frames.empty() && !taken.boneyards_planets && !taken.boneyards_pick &&
           !taken.boneyards_open && taken.boneyards_say.empty() && !taken.boneyards_chat &&
           taken.boneyards_create.empty() && !taken.boneyards_launch && !taken.boneyards_features &&
           !taken.check_boneyards_unit_limit && !taken.boneyards_overlay &&
           taken.boneyards_entry == BoneyardsEntryCheck::none && !taken.check_strip_buttons &&
           !taken.check_menu_modes;
}

/// Copies the runtime's player folder for a file that is opened or created.
///
/// An empty folder, or one that does not fit, is reported empty.
///
/// @param context query context (unused)
/// @param[out] out buffer for the folder
/// @param capacity size of `out` in bytes, including the terminating NUL
void copy_player_folder(void* /*context*/, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0)
        return;
    out[0] = '\0';
    if (g_player_folder_runtime == nullptr)
        return;
    try {
        const std::string text = oa::app::player_folder(*g_player_folder_runtime).string();
        if (text.size() + 1 > capacity)
            return;
        for (std::size_t i = 0; i < text.size(); ++i)
            out[i] = text[i];
        out[text.size()] = '\0';
    } catch (const std::exception&) {
        out[0] = '\0';
    }
}

// The expansion folder last resolved, so a file open does not read the
// choice again. Empty user folder is not kept: it is filled after startup.
struct ExpansionFolderHeld {
    bool known = false;
    std::string user;
    std::string content;
    std::string folder;
};

ExpansionFolderHeld g_expansion_held;

/// Copies the turned-on expansion folder for a file that is opened.
///
/// An empty folder, or one that does not fit, is reported empty. The choice
/// is read once the player's folder is known.
///
/// @param context query context (unused)
/// @param[out] out buffer for the folder
/// @param capacity size of `out` in bytes, including the terminating NUL
void copy_expansion_folder(void* /*context*/, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0)
        return;
    out[0] = '\0';
    if (g_player_folder_runtime == nullptr)
        return;
    try {
        const std::filesystem::path user_path = oa::app::player_folder(*g_player_folder_runtime);
        const std::string user = oa::platform::path_to_utf8(user_path);
        const Options& options = runtime_options(*g_player_folder_runtime);
        if (g_expansion_held.known && g_expansion_held.user == user &&
            g_expansion_held.content == options.content) {
            if (g_expansion_held.folder.size() + 1 > capacity)
                return;
            for (std::size_t i = 0; i < g_expansion_held.folder.size(); ++i)
                out[i] = g_expansion_held.folder[i];
            out[g_expansion_held.folder.size()] = '\0';
            return;
        }
        if (user.empty())
            return;
        oa::platform::preferences::Values values;
        if (!options.unattended || options.preferences_file)
            values = oa::platform::preferences::load(preference_file(options.preferences_file));
        const std::filesystem::path folder =
            installed_expansion_folder(user_path, values, options.content);
        g_expansion_held.known = true;
        g_expansion_held.user = user;
        g_expansion_held.content = options.content;
        g_expansion_held.folder = oa::platform::path_to_utf8(folder);
        if (g_expansion_held.folder.size() + 1 > capacity)
            return;
        for (std::size_t i = 0; i < g_expansion_held.folder.size(); ++i)
            out[i] = g_expansion_held.folder[i];
        out[g_expansion_held.folder.size()] = '\0';
    } catch (const std::exception&) {
        out[0] = '\0';
    }
}

} // namespace

bool boneyards_expansion_folder_refreshed() {
    // The longest folder the Boneyards' file query hands over, as it does.
    constexpr std::size_t folder_capacity = 4096;
    g_expansion_held = {};
    char folder[folder_capacity];
    copy_expansion_folder(nullptr, folder, sizeof folder);
    return folder[0] != '\0';
}

namespace {

/// The folder laid over a named game folder while its archives are found.
///
/// @param user_folder the player's folder
/// @param values the preferences
/// @param content the `--content` value
/// @return the expansion folder, or empty
std::filesystem::path expansion_files_overlay(
    const std::filesystem::path& user_folder,
    const oa::platform::preferences::Values& values,
    std::string_view content
) {
    return installed_expansion_folder(user_folder, values, content);
}

/// Returns the clock the Boneyards' idle work follows, in milliseconds.
///
/// This is the engine's extension clock. While the fixed clock is on, and
/// in a run a program on this machine drives, it advances by that clock's
/// step and does not read the wall clock. Otherwise it is the steady clock.
///
/// @param runtime the running app
/// @return the clock, in milliseconds
uint32_t idle_clock_ms(const Runtime& runtime) {
    return extension_clock(runtime);
}

// How many windows and controls one answer keeps. The endpoint keeps the
// same counts and drops the rest. 1024 holds every planet twice (its name
// and its id) plus the consoles.
constexpr std::size_t k_max_driver_windows = 32;
constexpr std::size_t k_max_driver_controls = 1024;

/// Maps a toolkit control onto the kind the driver names.
///
/// @param kind the toolkit's kind
/// @return the driver's kind
ExtensionControlKind driver_kind(oa::boneyards::DriverControlKind kind) {
    switch (kind) {
    case oa::boneyards::DriverControlKind::button:
        return ExtensionControlKind::button;
    case oa::boneyards::DriverControlKind::check_box:
        return ExtensionControlKind::check_box;
    case oa::boneyards::DriverControlKind::list:
        return ExtensionControlKind::list;
    case oa::boneyards::DriverControlKind::text_field:
        return ExtensionControlKind::text_field;
    case oa::boneyards::DriverControlKind::slider:
        return ExtensionControlKind::slider;
    case oa::boneyards::DriverControlKind::label:
        return ExtensionControlKind::label;
    case oa::boneyards::DriverControlKind::area:
        return ExtensionControlKind::area;
    case oa::boneyards::DriverControlKind::image:
        return ExtensionControlKind::image;
    }
    return ExtensionControlKind::button;
}

/// Appends one shown control to the window it sits on (DriverControlVisitor).
///
/// @param context the windows being filled
/// @param window_name the console or panel the control sits on
/// @param control the control
void append_driver_control(
    void* context, const char* window_name, const oa::boneyards::DriverControlView& control
) {
    if (context == nullptr || window_name == nullptr || window_name[0] == '\0' ||
        control.name == nullptr || control.name[0] == '\0')
        return;
    auto* windows = static_cast<std::vector<ExtensionWindow>*>(context);
    std::size_t controls = 0;
    ExtensionWindow* found = nullptr;
    for (ExtensionWindow& window : *windows) {
        controls += window.controls.size();
        if (found == nullptr && window.name == window_name)
            found = &window;
    }
    if (controls >= k_max_driver_controls)
        return;
    if (found == nullptr) {
        if (windows->size() >= k_max_driver_windows)
            return;
        windows->push_back(ExtensionWindow{});
        found = &windows->back();
        found->name = window_name;
    }
    ExtensionControl item;
    item.name = control.name;
    item.kind = driver_kind(control.kind);
    item.x = control.x;
    item.y = control.y;
    item.width = control.width;
    item.height = control.height;
    item.visible = control.visible;
    item.enabled = control.enabled;
    item.focused = control.focused;
    item.checked = control.checked;
    item.masked = control.masked;
    item.text_length = control.text_length > 0 ? control.text_length : 0;
    if (control.masked)
        item.text.clear();
    else
        item.text = control.text != nullptr ? control.text : "";
    if (control.kind == oa::boneyards::DriverControlKind::list && control.items != nullptr) {
        const int32_t count = control.item_count > 0 ? control.item_count : 0;
        item.items.reserve(static_cast<std::size_t>(count));
        for (int32_t index = 0; index < count; ++index) {
            const char* row = control.items[index];
            item.items.emplace_back(row != nullptr ? row : "");
        }
        item.first_visible = control.first_visible;
        item.rows = control.rows;
        item.row_height = control.row_height;
        item.selected = control.selected;
    }
    item.scroll_up = control.scroll_up != nullptr ? control.scroll_up : "";
    item.scroll_down = control.scroll_down != nullptr ? control.scroll_down : "";
    found->controls.push_back(std::move(item));
}

/// Copies a C string into a fixed buffer, cut to fit.
///
/// @param[out] out the buffer
/// @param capacity its size, including the terminator
/// @param text the text; null copies nothing
void copy_c_text(char* out, std::size_t capacity, const char* text) {
    if (capacity == 0)
        return;
    std::size_t index = 0;
    if (text != nullptr) {
        while (index + 1 < capacity && text[index] != '\0') {
            out[index] = text[index];
            ++index;
        }
    }
    out[index] = '\0';
}

/// Appends one synthetic control. The strings are copied before this returns.
///
/// @param[in,out] windows the windows being filled
/// @param window_name the console or panel
/// @param name the control's name
/// @param kind what it is
/// @param x its left edge
/// @param y its top edge
/// @param width its width; 0 has no area
/// @param height its height; 0 has no area
/// @param text its text
void append_plain(
    std::vector<ExtensionWindow>& windows,
    const char* window_name,
    const char* name,
    oa::boneyards::DriverControlKind kind,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const char* text
) {
    oa::boneyards::DriverControlView shown{};
    shown.name = name;
    shown.kind = kind;
    shown.x = x;
    shown.y = y;
    shown.width = width;
    shown.height = height;
    shown.visible = true;
    shown.enabled = kind != oa::boneyards::DriverControlKind::label;
    shown.text = text != nullptr ? text : "";
    shown.selected = -1;
    append_driver_control(&windows, window_name, shown);
}

/// Adds planet facts, the sector, the magnification and the advert banner.
///
/// A planet on the viewer is already a button. This also lists it by its
/// decimal id, on the same rectangle, so a click by id lands on it. A planet
/// off the viewer is a label with no area, under the main frame, by its
/// label and by its id: its facts can be read, and a click is refused until
/// it is on the viewer. The sector and the magnification are labels with no
/// area. The banner is a button over the picture the walker skips.
///
/// @param[in,out] windows the windows being filled
/// @param widget the status widget; null adds nothing
void append_lobby_facts(
    std::vector<ExtensionWindow>& windows, oa::boneyards::console::StatusWidget* widget
) {
    namespace console = oa::boneyards::console;
    console::DriverPlanetFact facts[oa::boneyards::screens::kMaxGalaxyPlanets];
    const std::size_t count = console::boneyards_status_widget_planet_facts(
        widget, facts, oa::boneyards::screens::kMaxGalaxyPlanets
    );
    for (std::size_t index = 0; index < count; ++index) {
        const console::DriverPlanetFact& fact = facts[index];
        char id_text[16];
        std::snprintf(id_text, sizeof id_text, "%d", static_cast<int>(fact.id));
        if (fact.on_viewer) {
            append_plain(
                windows,
                "MapPanel",
                id_text,
                oa::boneyards::DriverControlKind::button,
                fact.x,
                fact.y,
                fact.width,
                fact.height,
                fact.text
            );
        } else {
            append_plain(
                windows,
                "BYMainFrame",
                fact.label,
                oa::boneyards::DriverControlKind::label,
                0,
                0,
                0,
                0,
                fact.text
            );
            append_plain(
                windows,
                "BYMainFrame",
                id_text,
                oa::boneyards::DriverControlKind::label,
                0,
                0,
                0,
                0,
                fact.text
            );
        }
    }
    console::DriverMapCaption map_caption{};
    if (console::boneyards_status_widget_map_caption(widget, &map_caption) && map_caption.ready) {
        append_plain(
            windows,
            "BYMainFrame",
            "Sector",
            oa::boneyards::DriverControlKind::label,
            0,
            0,
            0,
            0,
            map_caption.sector
        );
        char magnification[16];
        std::snprintf(
            magnification, sizeof magnification, "%d", static_cast<int>(map_caption.magnification)
        );
        append_plain(
            windows,
            "BYMainFrame",
            "Magnification",
            oa::boneyards::DriverControlKind::label,
            0,
            0,
            0,
            0,
            magnification
        );
    }
    console::DriverBannerFact banner{};
    if (console::boneyards_status_widget_banner(widget, &banner) && banner.shown) {
        append_plain(
            windows,
            "BYMainFrame",
            "Ad_Banner",
            oa::boneyards::DriverControlKind::button,
            banner.x,
            banner.y,
            banner.width,
            banner.height,
            banner.identity
        );
    }
}

/// The short name posted when a console opens. Unknown ids post nothing.
///
/// @param id the console screen id
/// @return the name, or null
const char* console_note_name(int32_t id) {
    namespace screen = oa::boneyards::console::console_screen;
    switch (id) {
    case screen::update:
        return "update";
    case screen::login:
        return "login";
    case screen::recon:
        return "recon";
    case screen::chat:
        return "chat";
    case screen::battle:
        return "battle";
    case screen::ladders:
        return "ladders";
    case screen::news:
        return "news";
    case screen::profile:
        return "profile";
    case screen::help:
        return "help";
    case screen::quickplay:
        return "quickplay";
    case screen::locator:
        return "locator";
    default:
        return nullptr;
    }
}

/// Posts a session or console note when the lobby changes.
///
/// The first successful read is remembered and not posted. A later change of
/// the connection or the login name posts `session` as the level and the
/// name. A console posts `console` once it shows, unless it is the one last
/// posted and has not closed since.
///
/// @param runtime the running app
void note_lobby_frame(Runtime& runtime) {
    oa::boneyards::console::MainFrameProbe probe{};
    if (!oa::boneyards::console::boneyards_status_widget_probe(
            boneyards_console_status_widget(), &probe
        )) {
        g_lobby_seen = {};
        return;
    }
    namespace console_ids = oa::boneyards::console::console_screen;
    if (!g_lobby_seen.seeded) {
        g_lobby_seen.seeded = true;
        g_lobby_seen.connection_level = probe.connection_level;
        g_lobby_seen.console_screen = probe.screen_shown ? probe.console_screen : console_ids::none;
        copy_c_text(g_lobby_seen.user_name, sizeof g_lobby_seen.user_name, probe.user_name);
        return;
    }
    // Losing the Boneyards after the player has left drops the battle tunnel.
    // The battle room, the load into the match and the match itself keep it:
    // in_match is set only once the match has started, and the launch still
    // counts through that handover.
    if (g_lobby_seen.connection_level >=
            static_cast<int32_t>(oa::boneyards::SessionLevel::logged_in) &&
        probe.connection_level < static_cast<int32_t>(oa::boneyards::SessionLevel::logged_in))
        boneyards_overlay_lost(g_exit_prompt.in_match || boneyards_launched_game());
    if (probe.connection_level != g_lobby_seen.connection_level ||
        std::strcmp(probe.user_name, g_lobby_seen.user_name) != 0) {
        char text[oa::boneyards::kSessionNameCapacity + 16];
        std::snprintf(
            text, sizeof text, "%d %s", static_cast<int>(probe.connection_level), probe.user_name
        );
        post_extension_note(runtime, "session", text);
    }
    if (probe.screen_shown && probe.console_screen != g_lobby_seen.console_screen) {
        if (const char* name = console_note_name(probe.console_screen))
            post_extension_note(runtime, "console", name);
        g_lobby_seen.console_screen = probe.console_screen;
    } else if (probe.console_screen == console_ids::none) {
        g_lobby_seen.console_screen = console_ids::none;
    }
    g_lobby_seen.connection_level = probe.connection_level;
    copy_c_text(g_lobby_seen.user_name, sizeof g_lobby_seen.user_name, probe.user_name);
}

/// Forwards a lobby note onto the running app. A quiet load posts nothing.
///
/// @param kind the kind
/// @param text the note
void forward_driver_note(const char* kind, const char* text) {
    if (g_quiet_load || g_player_folder_runtime == nullptr)
        return;
    post_extension_note(*g_player_folder_runtime, kind, text);
}

/// Keeps the main menu's strip in the listing to what the menu shows.
///
/// With the strip's rows not shown (button and none modes) its buttons take
/// no click, so they are left out. In button mode ENTER is listed where it
/// stands on the plate.
///
/// @param[in,out] windows the consoles being listed
void list_menu_entry_as_shown(std::vector<ExtensionWindow>& windows) {
    namespace console = oa::boneyards::console;
    console::MenuEntryMode mode = console::MenuEntryMode::strip;
    oa::ui::frontend_renderer::SourceRect place{};
    if (boneyards_console_up() ||
        !boneyards_console_menu_entry(&mode, &place.x, &place.y, &place.width, &place.height) ||
        mode == console::MenuEntryMode::strip)
        return;
    for (ExtensionWindow& window : windows) {
        if (window.name != "MainMenuOverlay")
            continue;
        std::vector<ExtensionControl> shown;
        for (ExtensionControl& control : window.controls) {
            if (control.name != "EnterBYBtn" || place.width == 0)
                continue;
            control.x += place.x - console::entry_plate.x;
            control.y += place.y - console::entry_plate.y;
            shown.push_back(std::move(control));
        }
        window.controls = std::move(shown);
    }
}

/// Lists the consoles the Boneyards is showing (ExtensionWindowSource).
///
/// The ticker reads its whole line. While the map is locked (the green layer
/// slides or covers it, or a console is open), a press on a planet selects
/// nothing, so each planet on the viewer is listed as not taking a click.
/// On a main menu without the strip only the plate's ENTER is listed.
///
/// @param context unused
/// @param runtime the running app (unused)
/// @param[out] windows receives the consoles and their controls
void list_driver_windows(
    void* /*context*/, const Runtime& /*runtime*/, std::vector<ExtensionWindow>& windows
) {
    try {
        oa::boneyards::console::StatusWidget* widget = boneyards_console_status_widget();
        oa::boneyards::console::boneyards_status_widget_visit_driver_controls(
            widget, &windows, append_driver_control
        );
        append_lobby_facts(windows, widget);
        list_menu_entry_as_shown(windows);
        oa::boneyards::console::MainFrameProbe probe{};
        if (oa::boneyards::console::boneyards_status_widget_probe(widget, &probe)) {
            for (ExtensionWindow& window : windows) {
                const bool planets = window.name == "MapPanel";
                for (ExtensionControl& control : window.controls) {
                    if (control.name == "Ticker")
                        control.text = probe.ticker_text;
                    if (planets && probe.map_locked)
                        control.enabled = false;
                }
            }
        }
    } catch (const std::exception&) {
        windows.clear();
    }
}

/// Returns what the Boneyards box follows, as the game stands.
///
/// @return the frontend Game's mode, whether the Boneyards is up and whether a launch is active
ExitPromptObservation exit_prompt_observation() {
    ExitPromptObservation now{};
    now.frontend_mode = mp::multiplayer_game().mode;
    now.boneyards_up = boneyards_console_up();
    now.launched = api::launch_active();
    return now;
}

/// The page-font library opened for the Boneyards window. Null until that window opens.
oa::platform::FontLibrary* g_page_font_library = nullptr;

/// Gives the Boneyards markup pages a text service over the fonts shipped in the game's fonts/ folder.
///
/// The folder is in the one SDL_GetBasePath() names: beside the executable,
/// or in the application bundle's Resources on macOS. Called when the
/// Boneyards window opens, not while the main menu is up: the strip does not
/// draw these faces.
///
/// The font library is opened once per visit. When it cannot be opened the
/// error is reported on stderr and the pages stay in the FNT fonts.
///
/// @param widget the window's status widget; receives the text service
void install_boneyards_text_fonts(oa::boneyards::console::StatusWidget* widget) {
    if (g_page_font_library == nullptr) {
        const char* base = SDL_GetBasePath();
        const std::string directory =
            std::string(base != nullptr ? base : "") + oa::platform::k_bundled_fonts_directory;
        // The chain behind each page face lives in this folder too: DejaVu,
        // the engine's text faces and Noto Emoji. Chinese, Japanese and
        // Korean come from those faces. It is named even when the bundled
        // faces cannot be opened, so the FNT fonts can draw it.
        oa::boneyards::html::page_fonts_set_directory(directory.c_str());
        char error[0x200] = {};
        g_page_font_library =
            oa::platform::font_library_open(directory.c_str(), error, sizeof error);
        if (g_page_font_library == nullptr) {
            std::cerr << "error: " << error
                      << "; Boneyards pages fall back to the FNT fonts (reinstall the fonts/ "
                         "directory, or run tools/bootstrap_fonts.py and rebuild)\n";
            return;
        }
    }
    const oa::boneyards::html::FontService page_service =
        oa::boneyards::html::host_font_service(g_page_font_library);
    boneyards_console_set_text_fonts(page_service);
    oa::boneyards::console::boneyards_status_widget_set_text_fonts(widget, page_service);
}

/// Drops the page fonts and the HTML engine's faces opened for the Boneyards window.
///
/// @param widget the window's status widget; null only clears the library
void release_boneyards_text_fonts(oa::boneyards::console::StatusWidget* widget) {
    const oa::boneyards::html::FontService empty{};
    boneyards_console_set_text_fonts(empty);
    oa::boneyards::console::boneyards_status_widget_set_text_fonts(widget, empty);
    // Clearing the directory closes the shared DejaVu and emoji faces.
    oa::boneyards::html::page_fonts_set_directory(nullptr);
    oa::platform::font_library_close(g_page_font_library);
    g_page_font_library = nullptr;
}

/// Names the analogue atlas beside the bundled fonts.
///
/// The folder is the one SDL_GetBasePath() names, as for the pages' fonts:
/// beside the executable, or in the application bundle's Resources on macOS.
/// OA_FONT_ATLASES_DIR names another directory for a check. The console
/// reports a missing file and keeps drawing the faces' own glyphs.
void install_analogue_atlas() {
    std::string path;
    if (const auto dir = oa::platform::environment_value("OA_FONT_ATLASES_DIR");
        dir && !dir->empty()) {
        path = *dir + "/boneyards-analogues.bin";
    } else {
        const char* base = SDL_GetBasePath();
        path = std::string(base != nullptr ? base : "") +
               std::string(oa::platform::k_bundled_fonts_directory) + "/boneyards-analogues.bin";
    }
    boneyards_console_set_analogue_atlas_path(path.c_str());
}

/// Reports whether the system can open the registration address.
bool registration_browser_available() {
    return web_address_available();
}

/// Opens the registration address in the system browser.
bool registration_open_address(const char* address) {
    return open_web_address(address);
}

/// Reads one game file through the engine's read_game_file, then from the
/// archive that travels with the program.
///
/// The game's files are searched first, so a loose file or a mounted archive
/// wins over the copy beside the program. That copy supplies a path they do
/// not hold, which is how the new screens' layouts are found on a profile
/// that does not mount the mod. The war map is not in that archive. A missing
/// file returns false. The engine's call leaves `out` empty then, and it
/// does not throw.
///
/// @param context the runtime
/// @param path relative path, with '/' separators
/// @param[out] out receives the bytes
/// @return true when the file was read
bool read_virtual_file(void* context, const char* path, std::vector<uint8_t>* out) {
    const auto* runtime = static_cast<const Runtime*>(context);
    if (runtime == nullptr || path == nullptr || out == nullptr)
        return false;
    if (read_game_file(*runtime, path, *out))
        return true;
    return oa::boneyards::read_shipped_archive(path, *out);
}

/// Tells the system where the focused text field is, so characters being
/// composed can be offered beside the caret. A non-positive size clears it.
///
/// @param host the runtime the overlay was given; null does nothing
/// @param x left edge of the field, in screen pixels
/// @param y top edge of the field, in screen pixels
/// @param width field width; not positive clears the place
/// @param height field height; not positive clears the place
/// @param cursor caret distance from the field's left, in pixels
void place_focused_text_field(void* host, int x, int y, int width, int height, int cursor) {
    if (host == nullptr)
        return;
    auto& runtime = *static_cast<Runtime*>(host);
    if (width <= 0 || height <= 0) {
        set_focused_text_field(runtime, nullptr);
        return;
    }
    TextField field{};
    field.x = x;
    field.y = y;
    field.width = width;
    field.height = height;
    field.cursor = cursor;
    set_focused_text_field(runtime, &field);
}

/// Reads the simulation the running game is playing, for the launch and the report.
///
/// The engine's simulation_hash is that simulation: the profile in play,
/// with every Developer Mode override that changes it, or the plain 3.1c
/// hash. A hash that cannot be read cannot be confirmed. With no runtime
/// the facts are empty, which plays plain 3.1c.
///
/// @param context unused
/// @return the facts
DataFacts played_data_facts(void*) {
    DataFacts facts{};
    if (g_player_folder_runtime == nullptr)
        return facts;
    try {
        Runtime& runtime = *g_player_folder_runtime;
        facts.run_sim_hash = simulation_hash(runtime);
        if (facts.run_sim_hash.empty())
            facts.unreadable = true;
        const ActiveMod playing = active_mod(runtime);
        if (playing.active) {
            if (playing.has_profile)
                facts.profile_id = playing.profile_id;
            else
                facts.mod_folder = true;
        }
        facts.mutator_count = active_mutator_count(runtime);
    } catch (const std::exception&) {
        facts = DataFacts{};
        facts.unreadable = true;
    }
    return facts;
}

/// The strip's MULTI. With no multiplayer map the press shows that notice and
/// stays on the main menu, as the main menu's own MULTI does. A mod that
/// cannot start a game says so instead. Otherwise the strip opens multiplayer.
///
/// @param user unused
/// @return true when the press was answered here
bool answer_strip_multiplayer(void* /*user*/) {
    if (g_player_folder_runtime == nullptr)
        return false;
    return answer_multiplayer_entry(*g_player_folder_runtime);
}

/// Starts the online services before the screens register (Extension::startup).
///
/// Rover's session is configured with the game directory as its install
/// directory, where it finds ./boneyards, and with the --boneyards-server
/// list when one is given. The player's folder is asked for when a file is
/// opened or created: it is empty from this hook, and holds the folder once
/// the runtime has chosen it. A file the player has no copy of is read from
/// the game's files, through the engine's read_game_file, and then from the
/// archive that travels with the program. The page fonts stay
/// closed until the Boneyards window opens (the FNT fonts when they cannot
/// be opened). REGISTER's console opens its address through the engine's
/// web-address entry. A --boneyards-replay recording is installed next. Throws
/// std::runtime_error when the server list or the replay cannot be used.
///
/// @param context Extension context (unused).
/// @param runtime The runtime being built.
void startup(void* /*context*/, Runtime& runtime) {
    g_quiet_load = false;
    // Before the loop, so the inactive-loop check does not release after its
    // first frames, while the login console is still opening. Only when a
    // replay is given: the engine's own check has none, and releases after
    // its three frames. The login lets the release go once it has finished.
    if constexpr (self_checks_built) {
        if (inactive_login_requested(runtime))
            defer_inactive_loop_release(true);
    }
    // A headless --load never shows the main menu. The strip stays unbuilt.
    // With no request to open the Boneyards, the session and the screens are
    // left unset too, so the saved game does not keep their code.
    if (!runtime_options(runtime).load_file.empty() && runtime_options(runtime).headless_check)
        boneyards_console_defer_main_menu();
    if (quiet_saved_game(runtime)) {
        g_quiet_load = true;
        return;
    }
    const auto& options = boneyards_options();
    // Rover finds ./boneyards under the game directory, where the game runs.
    const std::string rover_install_dir =
        std::filesystem::absolute(runtime_options(runtime).game_dir).string();
    std::string rover_server_list;
    if (!options.boneyards_server.empty()) {
        std::string error;
        rover_server_list =
            oa::boneyards::rover_session_server_list(options.boneyards_server.string(), &error);
        if (rover_server_list.empty())
            throw std::runtime_error("--boneyards-server: " + error);
        rover_server_list = std::filesystem::absolute(rover_server_list).string();
    }
    oa::boneyards::rover_session_configure(rover_install_dir.c_str(), rover_server_list.c_str());
    g_expansion_held = {};
    g_player_folder_runtime = &runtime;
    oa::boneyards::console::boneyards_console_set_multiplayer_answer(
        answer_strip_multiplayer, nullptr
    );
    boneyards_set_data_facts_source(played_data_facts, nullptr);
    // The Profile's Unit Limit is the player's own setting, so it is saved.
    oa::boneyards::screens::profile_on_unit_limit([](int32_t units_per_player) {
        if (g_player_folder_runtime != nullptr)
            set_unit_limit(
                *g_player_folder_runtime, units_per_player, oa::app::SettingScope::immediate
            );
    });
    oa::boneyards::rover_session_set_player_folder_query(copy_player_folder, nullptr);
    oa::boneyards::rover_session_set_expansion_folder_query(copy_expansion_folder, nullptr);
    oa::boneyards::rover_session_set_virtual_files(
        read_virtual_file, const_cast<void*>(static_cast<const void*>(&runtime))
    );
    oa::boneyards::screens::registration_set_web_entry(
        oa::boneyards::screens::WebAddressEntry{
            registration_browser_available, registration_open_address
        }
    );
    oa::boneyards::console::boneyards_status_widget_set_font_lifetime(
        install_boneyards_text_fonts, release_boneyards_text_fonts
    );
    install_analogue_atlas();
    boneyards_console_set_text_field_place(place_focused_text_field);
    // The faces that travel with the game, and the player's text settings.
    // No chat is open yet; each pump gives the open one the same settings.
    oa::boneyards::modern_chat_use_engine();
    const GameTextPreferences text = game_text_preferences(runtime);
    oa::boneyards::console::chat_set_modern_text(text.modern_fonts, text.text_size);
    if (!options.boneyards_replay.empty()) {
        char error[0x200] = {};
        if (!oa::boneyards::rover_replay_install(
                options.boneyards_replay.string().c_str(), error, sizeof error
            ))
            throw std::runtime_error(std::string("--boneyards-replay: ") + error);
    }
    // The consoles publish their controls for a program driving the game,
    // and their notes (a line, a page, the ticker, an alert).
    set_extension_window_source(runtime, &g_player_folder_runtime, list_driver_windows);
    oa::boneyards::console::boneyards_console_set_driver_note(forward_driver_note);
    // Before the screens register, so the strip copies this flow.
    boneyards_entry_install(runtime);
    // How the way in stands on the main menu, as the mod's profile asks
    // (media.boneyards-entry: the mode, and the ENTER plate's place in button
    // mode), and the played mod's folder, the only one whose art is the
    // mod's; the strip reads them when it is first built. Without a profile
    // the record's own values are the baseline: auto, at the top right.
    const Options& run = runtime_options(runtime);
    const oa::data::mod_profile::MediaBoneyardsEntry entry =
        run.mod_profile != nullptr ? run.mod_profile->media.boneyards_entry
                                   : oa::data::mod_profile::MediaBoneyardsEntry{};
    boneyards_console_set_menu_entry(entry.mode.c_str(), entry.x, entry.y, run.layered_mod_folder);
}

/// Registers the Boneyards' screens, steps and queries (Extension::register_screens).
///
/// The main-menu strip registers with its steps (the main menu's shut-down
/// and return, the return after a launched battle and the entry "-y"
/// takes), then the console takes the bridge of a launched battle, the
/// frontend's questions about a launched battle are answered, and with
/// --check-boneyards-launch the overlay that plays a launched battle through
/// registers. After "-y" the strip's first tick asks for the frontend pass
/// whose main-menu update enters the Boneyards.
///
/// @param context Extension context (unused).
/// @param[in,out] registry Screen registry of the app.
void register_screens(void* /*context*/, ScreenRegistry* registry) {
    // The saved game never shows these screens, and startup left them unset.
    if (g_quiet_load)
        return;
    register_boneyards_console_screens(registry);
    BoneyardsLaunch launch{};
    launch.exit_prompt = &g_exit_prompt;
    bind_boneyards_launch(launch);
    boneyards_console_set_launch_bridge(boneyards_launch_bridge());
    register_boneyards_queries(registry);
    if (boneyards_options().setup_requested) {
        boneyards_console_request_entry_pass();
        boneyards_console_end_on_leave();
    }
    if (boneyards_options().check_boneyards_shortcut)
        boneyards_console_check_shortcut();
    // The launch, features and overlay checks are self-checks. A release
    // build does not compile them, so this branch is left out with them.
    if constexpr (self_checks_built) {
        if (boneyards_options().boneyards_launch)
            register_boneyards_launch_check(registry, *boneyards_options().boneyards_launch);
        if (boneyards_options().boneyards_features)
            register_boneyards_features_check(registry, *boneyards_options().boneyards_features);
        if (boneyards_options().boneyards_overlay)
            register_boneyards_overlay_check(registry, *boneyards_options().boneyards_overlay);
    }
}

/// Runs the Boneyards checks (Extension::run_mode).
///
/// At start --check-boneyards-login, or --check-boneyards-unit-limit, which
/// applies that limit from the Profile and reads it back from the engine's
/// unit limit preference; with the headless runs --enter-boneyards, followed
/// by a preferences write.
///
/// @param context Extension context (unused).
/// @param runtime The running app.
/// @param phase Point of Runtime::run offering the run.
/// @param[out] exit_code Receives the check's exit code when one ran.
/// @return True when a check ran, which ends Runtime::run.
bool run_mode(
    void* /*context*/,
    [[maybe_unused]] Runtime& runtime,
    [[maybe_unused]] RunPhase phase,
    [[maybe_unused]] int& exit_code
) {
    // A release build has no --check-* runs. The options may still name one;
    // nothing here carries it out, and the calls are not compiled.
    if constexpr (!self_checks_built)
        return false;
    return run_boneyards_self_checks(runtime, phase, exit_code);
}

/// Runs the Boneyards' part of --check-multiplayer-menu, after network play's (Extension::check_multiplayer_menu).
///
/// @param context Extension context (unused).
/// @param runtime The running app.
void check_multiplayer_menu(void* /*context*/, [[maybe_unused]] Runtime& runtime) {
    if constexpr (self_checks_built)
        check_boneyards_menu(check_host(runtime));
}

/// Runs the Boneyards' work of one frame stage (Extension::frame).
///
/// @param context Extension context (unused).
/// @param runtime The running app (unused).
/// @param stage pump follows the game into the Boneyards box, then runs the
///              Boneyards' idle work (the session is pumped every frame),
///              including while a remote-controlled option keeps the loop
///              running with the window inactive. After that pump, the
///              request that the main loop keep running while the window is
///              inactive is held from the moment a login starts (LOGIN
///              pressed, or "-y" opening the Boneyards) until the session
///              ends or the login fails. A live session keeps it, as before.
///              The request is written only when that answer changes.
///              --check-running-while-inactive with a replay drives a login
///              of its own and does not write the request. The engine's own
///              check sets the flag with no replay and is left to it.
///              after_pump does nothing.
///              presented, the call after the frame is shown, reads the
///              presented frame when --check-boneyards-overlay is set.
void frame(void* /*context*/, Runtime& runtime, FrameStage stage) {
    // A quiet saved-game load has no Boneyards window and no login, so this
    // pump does nothing.
    if (g_quiet_load)
        return;
    if (stage == FrameStage::presented) {
        boneyards_entry_on_frame(runtime, stage);
        if constexpr (self_checks_built) {
            if (boneyards_options().boneyards_overlay)
                present_boneyards_overlay(check_host(runtime), runtime_options(runtime));
        }
        return;
    }
    if (stage != FrameStage::pump)
        return;
    boneyards_entry_on_frame(runtime, stage);
    const GameTextPreferences text = game_text_preferences(runtime);
    oa::boneyards::console::chat_set_modern_text(text.modern_fonts, text.text_size);
    exit_prompt_observe(g_exit_prompt, exit_prompt_observation());
    boneyards_console_idle(idle_clock_ms(runtime));
    note_lobby_frame(runtime);
    boneyards_pump_overlay_route();
    // The idle work pumps the session, so this reads the login, logout or
    // drop that pump just applied. The loop decides whether to wait before
    // the next frame, and a hold taken here is what that decision reads.
    // The inactive-loop check owns the latch. Its login, when a replay is
    // given, runs here and the hold is not written. With no replay the
    // engine's own check counts its frames, and the hold is still not written.
    if constexpr (self_checks_built) {
        if (inactive_login_requested(runtime)) {
            run_boneyards_inactive_check(runtime);
            return;
        }
    }
    hold_background(runtime);
}

/// Answers a request to end the program with the Boneyards box "Exit to the system?" (Extension::close_requested).
///
/// While the box is installed it opens. When it is not installed, or cannot
/// open (the Boneyards' interface is down, as once the main menu has been
/// left for another screen), the request falls to the close handler network
/// play has installed (exit_prompt_close_requested).
///
/// @param context Extension context (unused).
/// @param runtime The running app (unused).
/// @return true when the box opened
bool close_requested(void* /*context*/, Runtime& /*runtime*/) {
    return exit_prompt_close_requested(
        g_exit_prompt, exit_prompt_observation(), boneyards_console_ask_exit
    );
}

/// Follows an application mode the frontend set into the Boneyards box (Extension::app_mode_set).
///
/// @param context Extension context (unused).
/// @param runtime The running app (unused).
/// @param mode The application mode set.
void app_mode_set(void* /*context*/, Runtime& /*runtime*/, int32_t mode) {
    exit_prompt_mode_set(g_exit_prompt, mode, exit_prompt_observation());
}

/// Notes a match started, which the Boneyards box follows (Extension::match_game).
///
/// The Boneyards writes nothing into the match's Game block.
///
/// @param context Extension context (unused).
/// @param game The new match's Game block (unused).
void match_game(void* /*context*/, oa::Game& /*game*/) {
    exit_prompt_match_started(g_exit_prompt);
}

/// Notes a match that finished, was left or was torn down, which the Boneyards box follows (Extension::match_event).
///
/// @param context Extension context (unused).
/// @param runtime The running app (unused).
/// @param event What happened to the match.
void match_event(void* /*context*/, Runtime& /*runtime*/, MatchEvent event) {
    // boneyards_match_event_leaves knows these values. A reorder has to change it.
    static_assert(static_cast<uint8_t>(MatchEvent::finished) == 0);
    static_assert(static_cast<uint8_t>(MatchEvent::torn_down) == 1);
    static_assert(static_cast<uint8_t>(MatchEvent::left) == 2);
    static_assert(static_cast<uint8_t>(MatchEvent::watching_kept) == 5);
    // The box follows finished, torn_down and left. The tunnel does not follow
    // torn_down: a new battle's setup destroys any match already there,
    // including none, before that battle starts. watching_kept is neither.
    if (event == MatchEvent::finished || event == MatchEvent::torn_down ||
        event == MatchEvent::left)
        exit_prompt_match_ended(g_exit_prompt);
    if (boneyards_match_event_leaves(static_cast<uint8_t>(event)))
        boneyards_match_ended();
}

/// Keeps the stored password while the Boneyards launched the game (Extension::keep_stored_password).
///
/// @param context Extension context (unused).
/// @return Whether the Boneyards launched the game.
bool keep_stored_password(void* /*context*/) {
    return boneyards_launched_game();
}

/// Returns the launch's label after a Boneyards launch (Extension::return_label).
///
/// @param context Extension context (unused).
/// @return "Boneyards" while the Boneyards launched the game and the launch
///         is active; null otherwise
const char* return_label(void* /*context*/) {
    return boneyards_return_label();
}

/// Frees what the Boneyards keeps for a runtime that is being destroyed (Extension::release_runtime).
///
/// The overlay and the graphics it loaded are kept for the runtime that
/// built them, and named by no runtime, so they are freed here. Nothing of
/// the runtime is used. It must not throw.
///
/// @param context Extension context (unused).
/// @param runtime The runtime being destroyed. Only its address is used, to
///                drop the player-folder query and the file reader that name it.
void release_runtime(void* /*context*/, Runtime& runtime) {
    boneyards_runtime_released();
    oa::boneyards::console::boneyards_console_set_driver_note(nullptr);
    g_lobby_seen = {};
    if (g_player_folder_runtime == &runtime) {
        set_extension_window_source(runtime, &g_player_folder_runtime, nullptr);
        g_player_folder_runtime = nullptr;
        oa::boneyards::screens::profile_on_unit_limit(nullptr);
        oa::boneyards::rover_session_set_player_folder_query(nullptr, nullptr);
        oa::boneyards::rover_session_set_expansion_folder_query(nullptr, nullptr);
        oa::boneyards::rover_session_set_virtual_files(nullptr, nullptr);
        g_expansion_held = {};
    }
    boneyards_console_set_text_field_place(nullptr);
    // Back on the Boneyards' fonts before the console is freed, then drop
    // the faces. The next runtime installs its own.
    oa::boneyards::console::chat_set_modern_text(false, oa::app::default_game_text_size);
    oa::boneyards::modern_chat_set_faces(nullptr);
    boneyards_console_release();
    // A window that never finished closing, or a runtime that built no overlay.
    release_boneyards_text_fonts(nullptr);
    // The request lives on the runtime that is going away. The next runtime
    // starts released, and a live session holds again on its first pump.
    g_background_held = false;
    g_login_background = {};
    g_quiet_load = false;
}

} // namespace

/// Asks the game folder's inspection to lay the expansion folder over it.
void register_expansion_overlay() {
    set_game_files_overlay(expansion_files_overlay);
}

} // namespace oa::app

/// Fills the Boneyards' extension table (its oa_add_extension INIT) and network play's hooks.
///
/// @param[out] table the Boneyards' table, zeroed on entry
void oa_extension_init_boneyards(oa::app::Extension* table) {
    namespace app = oa::app;
    *table = {};
    app::fill_boneyards_option_hooks(*table, app::boneyards_option_context());
    table->startup = app::startup;
    table->register_screens = app::register_screens;
    table->run_mode = app::run_mode;
    table->check_multiplayer_menu = app::check_multiplayer_menu;
    table->frame = app::frame;
    table->close_requested = app::close_requested;
    table->app_mode_set = app::app_mode_set;
    table->match_game = app::match_game;
    table->match_event = app::match_event;
    table->keep_stored_password = app::keep_stored_password;
    table->return_label = app::return_label;
    table->release_runtime = app::release_runtime;
    oa::app::netgame::extension_api::Hooks hooks{};
    app::fill_boneyards_report_hooks(hooks);
    hooks.page = app::boneyards_page;
    hooks.join_failed = app::boneyards_join_failed;
    oa::app::netgame::extension_api::set_hooks(hooks);
    oa::app::register_expansion_overlay();
}
