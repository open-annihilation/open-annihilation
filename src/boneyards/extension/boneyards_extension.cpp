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
#include "oa/boneyards/screens/after_battle.hpp"
#include "oa/boneyards/screens/galaxy.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
#include "oa/boneyards/screens/profile.hpp"
#include "oa/boneyards/screens/registration.hpp"
#include "oa/boneyards/shown_text.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/fonts.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/platform/system.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
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
// is Kit 2's look in the kit's headers. Version 20 adds ExtensionControl's
// masked, text_length, scroll_up and scroll_down members, and the calls
// answer_multiplayer_entry, defer_inactive_loop_release,
// post_extension_note and take_extension_notes. Version 21 adds
// accessibility fields on controls, the replay viewer's eight hooks, and
// the rectangle where an extension window's art takes the press. The strip
// states that rectangle while its art is drawn. Version 21 also adds
// ScreenServices::main_menu_order_owns_key. The main-menu overlay calls it
// and passes the key on when the host owns it. Version 21 also adds
// ScreenServices::bound_focus_action, which the multiplayer screens call, and
// spoken_updates_open, which this module asks before it composes a spoken
// line. This extension table fills no new hook. None of 15 to 21 is a hook
// this module fills.
constexpr uint32_t kExtensionApiVersionFollowed = 21;
static_assert(
    oa::app::extension_api_version == kExtensionApiVersionFollowed,
    "the engine's extension table changed: follow its change, then raise "
    "kExtensionApiVersionFollowed"
);
// Version 4 of network play's API adds recorded_game, which the Boneyards
// fills (boneyards_recorded_game): a battle's replay carries its game id,
// planet and sector.
constexpr uint32_t kNetworkPlayApiVersionFollowed = 4;
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
void boneyards_console_exit_to_menu();
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
// Runtime is incomplete here. These reach its methods from the files that
// define them.
void snapshot_presented_stage(Runtime& runtime, const char* stage);
void queue_battle_replay_share(Runtime& runtime);
[[nodiscard]] bool battle_has_recording(const Runtime& runtime);

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
    char selected_name[16]{};
    bool summary_shown{};
    char summary_name[16]{};
    char summary_owner[16]{};
    char summary_battles[12]{};
    char summary_forming[12]{};
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
    // The window's own focus, copied before a reported first stop is named.
    item.holds_focus = control.focused;
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

/// The Boneyards catalogue beside the program, or an empty one.
///
/// The engine does not read this file. A missing file leaves every sentence
/// in English.
///
/// @return the catalogue; it lives for the process
const oa::data::languages::InterfaceText& speech_words() {
    static const oa::data::languages::InterfaceText catalogue = [] {
        oa::data::languages::InterfaceText loaded;
        const std::filesystem::path file =
            oa::platform::path_from_utf8(oa::platform::program_directory()) / "languages" /
            "boneyards.tdf";
        std::ifstream input(file, std::ios::binary);
        if (!input)
            return loaded;
        const std::string bytes(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()
        );
        loaded.add(bytes, nullptr);
        return loaded;
    }();
    return catalogue;
}

/// One catalogue sentence in the language the interface is showing.
///
/// @param english the sentence as the source writes it
/// @return the translation, or the English when the catalogue has none
std::string speech(const char* english) {
    if (english == nullptr || english[0] == '\0')
        return {};
    const std::string_view line =
        speech_words().text(english, oa::data::languages::interface_language());
    return std::string(line);
}

/// Fills `{token}` places in a catalogue sentence.
///
/// @param english the pattern
/// @param places each place name and the text that replaces it
/// @return the filled sentence
std::string speech_filled(
    const char* english, std::initializer_list<std::pair<std::string_view, std::string_view>> places
) {
    std::string pattern = speech(english);
    for (const auto& place : places) {
        const std::string token = std::string("{") + std::string(place.first) + "}";
        std::string::size_type at = 0;
        while ((at = pattern.find(token, at)) != std::string::npos) {
            pattern.replace(at, token.size(), place.second);
            at += place.second.size();
        }
    }
    return pattern;
}

/// The room joins and leaves name. Entering a room replaces it.
///
/// @return the room
std::string& spoken_room() {
    static std::string room = "Main";
    return room;
}

/// Posts one internal spoken line. `S` is a login, `P` waits, `A` does not.
/// `G` is a page and `W` a whisper, which follow speak-chat.
///
/// @param runtime the running app
/// @param prefix `S`, `P`, `A`, `G` or `W`
/// @param sentence the sentence, already in the player's language
void post_spoken_line(Runtime& runtime, char prefix, const std::string& sentence) {
    if (!spoken_updates_open() || sentence.empty())
        return;
    std::string text;
    text.push_back(prefix);
    text.push_back('\t');
    text += sentence;
    post_extension_note(runtime, "line", text.c_str());
}

/// Posts a join or a leave that may collapse with the room's chat.
///
/// @param runtime the running app
/// @param room the room
/// @param speaker the player
/// @param sentence the sentence
void post_spoken_membership(
    Runtime& runtime,
    const std::string& room,
    const std::string& speaker,
    const std::string& sentence
) {
    if (!spoken_updates_open() || sentence.empty())
        return;
    const std::string text = std::string("C\t") + room + "\t" + speaker + "\t" + sentence;
    post_extension_note(runtime, "line", text.c_str());
}

/// The English title of a console id.
///
/// @param id the short name the console note carries
/// @return the title, or null
const char* console_english(const char* id) {
    if (std::strcmp(id, "update") == 0)
        return "Update";
    if (std::strcmp(id, "login") == 0)
        return "Login";
    if (std::strcmp(id, "recon") == 0)
        return "Recon";
    if (std::strcmp(id, "chat") == 0)
        return "Chat";
    if (std::strcmp(id, "battle") == 0)
        return "Battle";
    if (std::strcmp(id, "ladders") == 0)
        return "Ladders";
    if (std::strcmp(id, "news") == 0)
        return "News";
    if (std::strcmp(id, "profile") == 0)
        return "Profile";
    if (std::strcmp(id, "help") == 0)
        return "Help";
    if (std::strcmp(id, "quickplay") == 0)
        return "QuickPlay";
    if (std::strcmp(id, "locator") == 0)
        return "Locator";
    return nullptr;
}

/// Posts "Planet {name}".
///
/// @param runtime the running app
/// @param name the planet
void post_spoken_place(Runtime& runtime, const char* name) {
    if (!spoken_updates_open() || name == nullptr || name[0] == '\0')
        return;
    post_spoken_line(runtime, 'P', speech_filled("Planet {name}", {{"name", name}}));
}

/// Posts a planet's summary.
///
/// @param runtime the running app
/// @param name the planet
/// @param owner its owner
/// @param battles how many battles
/// @param forming how many are forming
void post_spoken_summary(
    Runtime& runtime, const char* name, const char* owner, const char* battles, const char* forming
) {
    if (!spoken_updates_open() || name == nullptr || name[0] == '\0')
        return;
    post_spoken_line(
        runtime,
        'P',
        speech_filled(
            "Summary of {name}: {owner}, {battles} battles, {forming} forming",
            {{"name", name},
             {"owner", owner != nullptr ? owner : ""},
             {"battles", battles != nullptr ? battles : ""},
             {"forming", forming != nullptr ? forming : ""}}
        )
    );
}

/// Posts the connection sentence. A login is said once by the engine.
///
/// @param runtime the running app
/// @param level the session level
/// @param name the player, when one is logged in
void post_spoken_session(Runtime& runtime, int level, const char* name) {
    if (!spoken_updates_open())
        return;
    std::string sentence;
    if (level <= 0)
        sentence = speech("Offline");
    else if (level == 1)
        sentence = speech("Connecting");
    else if (level == 2)
        sentence = speech("Disconnected");
    else if (name != nullptr && name[0] != '\0')
        sentence = speech_filled("Logged in as {name}", {{"name", name}});
    post_spoken_line(runtime, 'S', sentence);
}

/// Posts the console sentence, in the catalogue's words for that console.
///
/// @param runtime the running app
/// @param id the console note's short name
void post_spoken_console(Runtime& runtime, const char* id) {
    if (!spoken_updates_open())
        return;
    const char* english = console_english(id);
    if (english == nullptr)
        return;
    if (std::strcmp(id, "battle") == 0 && g_lobby_seen.selected_name[0] != '\0') {
        post_spoken_line(
            runtime, 'P', speech_filled("Battle, {place}", {{"place", g_lobby_seen.selected_name}})
        );
        return;
    }
    const std::string title = speech(english);
    post_spoken_line(runtime, 'P', speech_filled("Opened {console}", {{"console", title}}));
}

/// Reads `[Entered 'Room']`.
///
/// @param text the line
/// @param[out] room the room name
/// @return true when the line is that sentence
bool entered_room_line(const std::string& text, std::string& room) {
    constexpr std::string_view kOpen = "[Entered '";
    constexpr std::string_view kClose = "']";
    if (!text.starts_with(kOpen) || !text.ends_with(kClose) ||
        text.size() < kOpen.size() + kClose.size())
        return false;
    room.assign(text, kOpen.size(), text.size() - kOpen.size() - kClose.size());
    return true;
}

/// Reads `<Page from Name> text`.
///
/// @param text the line
/// @param[out] name the sender
/// @param[out] body the message
/// @return true when the line is a page
bool page_from_line(std::string_view text, std::string& name, std::string& body) {
    constexpr std::string_view kOpen = "<Page from ";
    if (!text.starts_with(kOpen))
        return false;
    const std::size_t end = text.find('>');
    if (end == std::string_view::npos || end < kOpen.size())
        return false;
    name.assign(text.substr(kOpen.size(), end - kOpen.size()));
    body.clear();
    if (end + 1 < text.size()) {
        std::string_view rest = text.substr(end + 1);
        if (rest.starts_with(" "))
            rest.remove_prefix(1);
        body.assign(rest);
    }
    return !name.empty();
}

/// Reads `<You whispered to Name> text`.
///
/// @param text the line
/// @param[out] name the other player
/// @param[out] body the message
/// @return true when the line is that echo
bool whisper_to(std::string_view text, std::string& name, std::string& body) {
    constexpr std::string_view kMark = "whispered to ";
    const std::size_t at = text.find(kMark);
    if (at == std::string_view::npos)
        return false;
    const std::size_t name_at = at + kMark.size();
    const std::size_t end = text.find('>', name_at);
    if (end == std::string_view::npos)
        return false;
    name.assign(text.substr(name_at, end - name_at));
    body.clear();
    if (end + 1 < text.size()) {
        std::string_view rest = text.substr(end + 1);
        if (rest.starts_with(" "))
            rest.remove_prefix(1);
        body.assign(rest);
    }
    return !name.empty();
}

/// Reads `Name: text`.
///
/// @param text the line
/// @param[out] name the speaker
/// @param[out] body the message
/// @return true when the line is ordinary chat
bool said_speaker(std::string_view text, std::string& name, std::string& body) {
    if (text.empty() || text.front() == '[' || text.front() == '<')
        return false;
    const std::size_t colon = text.find(": ");
    if (colon == std::string_view::npos || colon == 0)
        return false;
    name.assign(text.substr(0, colon));
    const std::size_t tag = name.find(" [");
    if (tag != std::string::npos)
        name.resize(tag);
    body.assign(text.substr(colon + 2));
    return !name.empty();
}

/// The message box's words, with its escaped newlines turned into breaks.
///
/// @param english the line the box was given
/// @return the words it shows
std::string box_words(const char* english) {
    const char* face = oa::boneyards::shown_text(english != nullptr ? english : "");
    std::string copy = face != nullptr ? face : "";
    std::string broken;
    broken.reserve(copy.size());
    for (std::size_t index = 0; index < copy.size(); ++index) {
        if (copy[index] == '\\' && index + 1 < copy.size() && copy[index + 1] == 'n') {
            broken.push_back('\n');
            ++index;
            continue;
        }
        broken.push_back(copy[index]);
    }
    return broken;
}

/// Posts the spoken form of one forwarded note. The raw note is posted too.
///
/// @param runtime the running app
/// @param kind the note's kind
/// @param text the note's text
void speak_forwarded(Runtime& runtime, const char* kind, const char* text) {
    if (!spoken_updates_open() || kind == nullptr || text == nullptr)
        return;
    const std::string line(text);
    if (std::strcmp(kind, "said") == 0) {
        std::string room;
        if (entered_room_line(line, room)) {
            if (!room.empty())
                spoken_room() = room;
            post_spoken_line(
                runtime, 'P', speech_filled("Entered {room}", {{"room", spoken_room()}})
            );
            return;
        }
        std::string name;
        std::string body;
        if (whisper_to(line, name, body)) {
            post_spoken_line(
                runtime,
                'W',
                speech_filled("Whisper to {name}: {text}", {{"name", name}, {"text", body}})
            );
            return;
        }
        if (page_from_line(line, name, body))
            return;
        if (!said_speaker(line, name, body))
            return;
        constexpr std::string_view kWhispered = "<Whispered>";
        if (body.starts_with(kWhispered)) {
            std::string_view rest(body);
            rest.remove_prefix(kWhispered.size());
            if (rest.starts_with(" "))
                rest.remove_prefix(1);
            const char* pattern =
                rest.empty() ? "Whisper from {name}" : "Whisper from {name}: {text}";
            post_spoken_line(
                runtime, 'W', speech_filled(pattern, {{"name", name}, {"text", rest}})
            );
        }
        return;
    }
    if (std::strcmp(kind, "page") == 0) {
        std::string name;
        std::string body;
        if (!page_from_line(line, name, body)) {
            post_spoken_line(runtime, 'G', line);
            return;
        }
        const char* pattern = body.empty() ? "Page from {name}" : "Page from {name}: {text}";
        post_spoken_line(runtime, 'G', speech_filled(pattern, {{"name", name}, {"text", body}}));
        return;
    }
    if (std::strcmp(kind, "alert") == 0) {
        std::string name;
        std::string body;
        if (page_from_line(line, name, body))
            return;
        post_spoken_line(runtime, 'A', box_words(text));
        return;
    }
    if (std::strcmp(kind, "room") != 0)
        return;
    constexpr std::string_view kJoined = "joined ";
    constexpr std::string_view kLeaving = "left ";
    const bool joined = line.starts_with(kJoined);
    const bool leaving = line.starts_with(kLeaving);
    if (!joined && !leaving)
        return;
    const std::string name = line.substr((joined ? kJoined : kLeaving).size());
    if (name.empty() || name == g_lobby_seen.user_name)
        return;
    const char* pattern = joined ? "{name} joined {room}" : "{name} left {room}";
    post_spoken_membership(
        runtime,
        spoken_room(),
        name,
        speech_filled(pattern, {{"name", name}, {"room", spoken_room()}})
    );
}

/// Remembers the selected planet and its summary, and posts them after the seed.
///
/// @param runtime the running app
/// @param probe the frame just read
/// @param post true to post a change; the seed only remembers
void remember_place(
    Runtime& runtime, const oa::boneyards::console::MainFrameProbe& probe, bool post
) {
    if (std::strcmp(probe.selected_name, g_lobby_seen.selected_name) != 0) {
        copy_c_text(
            g_lobby_seen.selected_name, sizeof g_lobby_seen.selected_name, probe.selected_name
        );
        if (post && g_lobby_seen.selected_name[0] != '\0') {
            post_extension_note(runtime, "place", g_lobby_seen.selected_name);
            post_spoken_place(runtime, g_lobby_seen.selected_name);
        }
    }
    const bool summary_changed =
        probe.summary_shown != g_lobby_seen.summary_shown ||
        std::strcmp(probe.summary_name, g_lobby_seen.summary_name) != 0 ||
        std::strcmp(probe.summary_owner, g_lobby_seen.summary_owner) != 0 ||
        std::strcmp(probe.summary_battles, g_lobby_seen.summary_battles) != 0 ||
        std::strcmp(probe.summary_forming, g_lobby_seen.summary_forming) != 0;
    if (!summary_changed)
        return;
    g_lobby_seen.summary_shown = probe.summary_shown;
    copy_c_text(g_lobby_seen.summary_name, sizeof g_lobby_seen.summary_name, probe.summary_name);
    copy_c_text(g_lobby_seen.summary_owner, sizeof g_lobby_seen.summary_owner, probe.summary_owner);
    copy_c_text(
        g_lobby_seen.summary_battles, sizeof g_lobby_seen.summary_battles, probe.summary_battles
    );
    copy_c_text(
        g_lobby_seen.summary_forming, sizeof g_lobby_seen.summary_forming, probe.summary_forming
    );
    if (!post || !probe.summary_shown || probe.summary_name[0] == '\0')
        return;
    char text[128];
    std::snprintf(
        text,
        sizeof text,
        "summary\t%s\t%s\t%s\t%s",
        probe.summary_name,
        probe.summary_owner,
        probe.summary_battles,
        probe.summary_forming
    );
    post_extension_note(runtime, "place", text);
    post_spoken_summary(
        runtime,
        probe.summary_name,
        probe.summary_owner,
        probe.summary_battles,
        probe.summary_forming
    );
}

/// Posts a session or console note when the lobby changes.
///
/// The first successful read is remembered and not posted. A later change of
/// the connection or the login name posts `session` as the level and the
/// name. A console posts `console` once it shows, unless it is the one last
/// posted and has not closed since. A planet and its summary post `place`.
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
        remember_place(runtime, probe, false);
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
        post_spoken_session(runtime, probe.connection_level, probe.user_name);
    }
    if (probe.screen_shown && probe.console_screen != g_lobby_seen.console_screen) {
        if (const char* name = console_note_name(probe.console_screen)) {
            post_extension_note(runtime, "console", name);
            post_spoken_console(runtime, name);
        }
        g_lobby_seen.console_screen = probe.console_screen;
    } else if (probe.console_screen == console_ids::none) {
        g_lobby_seen.console_screen = console_ids::none;
    }
    g_lobby_seen.connection_level = probe.connection_level;
    copy_c_text(g_lobby_seen.user_name, sizeof g_lobby_seen.user_name, probe.user_name);
    remember_place(runtime, probe, true);
}

/// Forwards a lobby note onto the running app. A quiet load posts nothing.
///
/// @param kind the kind
/// @param text the note
void forward_driver_note(const char* kind, const char* text) {
    if (g_quiet_load || g_player_folder_runtime == nullptr)
        return;
    post_extension_note(*g_player_folder_runtime, kind, text);
    speak_forwarded(*g_player_folder_runtime, kind, text);
}

/// The words a player hears for one English line.
///
/// The pointer from the catalogue is copied before the next line is asked for.
///
/// @param english the line as the source writes it
/// @return the line in the player's language
std::string spoken(const char* english) {
    const char* line = oa::boneyards::shown_text(english);
    return std::string(line != nullptr ? line : "");
}

/// A printf template in the player's language, or the English when a
/// translation asks for different arguments.
///
/// @param english the template as the source writes it
/// @return the template to fill
std::string spoken_pattern(const char* english) {
    char line[192];
    oa::boneyards::copy_shown_pattern(line, sizeof line, english);
    return std::string(line);
}

/// One picture button, named as its window is, and the words it speaks.
struct SpokenButton {
    const char* name{};
    const char* label{};
    const char* shortcut{};
    bool tab{};
};

/// Picture buttons and the words a player hears for them. The window names
/// stay as they are. A shortcut is a single letter, empty when it has none.
constexpr SpokenButton k_spoken_buttons[] = {
    {"MF_Release_Button", "Release", "", false},
    {"MF_Recon_Button", "Recon", "", false},
    {"MF_Chat_Button", "Chat", "", false},
    {"MF_Battle_Button", "Battle", "", false},
    {"MF_News_Button", "News", "", false},
    {"MF_Ladders_Button", "Ladders", "", false},
    {"MF_Locator_Button", "Locator", "", false},
    {"MF_Profile_Button", "Profile", "", false},
    {"MF_Help_Button", "Help", "", false},
    {"MF_Quickplay_Button", "QuickPlay", "", false},
    {"ExitBYBtn", "Exit", "", false},
    {"Map up", "Scroll the map up", "", false},
    {"Map up right", "Scroll the map up and right", "", false},
    {"Map right", "Scroll the map right", "", false},
    {"Map down right", "Scroll the map down and right", "", false},
    {"Map down", "Scroll the map down", "", false},
    {"Map down left", "Scroll the map down and left", "", false},
    {"Map left", "Scroll the map left", "", false},
    {"Map up left", "Scroll the map up and left", "", false},
    {"SingleBtn", "SINGLE", "S", false},
    {"SingleFormer", "SINGLE", "S", false},
    {"MultiBtn", "MULTI", "M", false},
    {"MultiFormer", "MULTI", "M", false},
    {"IntroBtn", "INTRO", "I", false},
    {"IntroFormer", "INTRO", "I", false},
    {"ExitBtn", "EXIT", "E", false},
    {"ExitFormer", "EXIT", "E", false},
    {"CreditsBtn", "Credits", "", false},
    {"EnterBYBtn", "Enter the Boneyards", "B", false},
    {"GeneralTabBtn", "General", "", true},
    {"LadderTabBtn", "Ladder", "", true},
    {"AwardsTabBtn", "Awards", "", true},
};

/// Tells whether a name is only decimal digits.
///
/// @param name the control's name
/// @return true when every character is a digit and there is at least one
bool digits_only(const std::string& name) {
    if (name.empty())
        return false;
    for (const char character : name) {
        if (character < '0' || character > '9')
            return false;
    }
    return true;
}

/// Reads a trailing index from a name such as "Pick0" or "row_3".
///
/// @param name the control's name
/// @param prefix the part before the index
/// @param[out] index the index
/// @return true when the name is that prefix and a decimal index
bool indexed_name(const std::string& name, const char* prefix, int* index) {
    if (prefix == nullptr || index == nullptr)
        return false;
    const std::size_t length = std::strlen(prefix);
    if (name.size() <= length || name.compare(0, length, prefix) != 0)
        return false;
    int value = 0;
    bool any = false;
    for (std::size_t at = length; at < name.size(); ++at) {
        const char character = name[at];
        if (character < '0' || character > '9')
            return false;
        any = true;
        value = value * 10 + (character - '0');
    }
    if (!any)
        return false;
    *index = value;
    return true;
}

/// The planet name a control speaks. A control named only by its id takes
/// the name of the control that shows the same facts.
///
/// @param window the console the control sits on
/// @param control the planet control
/// @return the name; empty when the control has none
std::string planet_label_of(const ExtensionWindow& window, const ExtensionControl& control) {
    if (!digits_only(control.name))
        return control.name;
    for (const ExtensionControl& other : window.controls) {
        if (other.text == control.text && !digits_only(other.name))
            return other.name;
    }
    return control.name;
}

/// Finds a control by its name.
///
/// @param window the console
/// @param name the name
/// @return the control, or null
const ExtensionControl* find_named(const ExtensionWindow& window, const std::string& name) {
    for (const ExtensionControl& control : window.controls) {
        if (control.name == name)
            return &control;
    }
    return nullptr;
}

/// The window a rank or latency icon is listed under: the list that holds the row.
///
/// @param icon the icon
/// @return the window's name, or null when it has none
const char* icon_window_name(const oa::boneyards::Window* icon) {
    for (const oa::boneyards::Window* at = icon != nullptr ? icon->parent : nullptr; at != nullptr;
         at = at->parent) {
        if (at->ops != nullptr && at->ops->class_name != nullptr &&
            std::strcmp(at->ops->class_name, oa::boneyards::screens::kByPlayerRowClass) == 0)
            continue;
        if (at->name != nullptr && at->name[0] != '\0')
            return at->name;
    }
    return nullptr;
}

/// The words a rank or latency icon speaks.
///
/// @param words what the icon is showing
/// @return the words, in the player's language; empty when it is neither
std::string icon_label(const oa::boneyards::screens::PlayerIconWords& words) {
    namespace screens = oa::boneyards::screens;
    if (words.kind == screens::PlayerIconKind::rank) {
        if (words.rank == nullptr || words.rank[0] == '\0')
            return spoken("Rank");
        const std::string title = spoken(words.rank);
        const std::string pattern = spoken_pattern("Rank: %s");
        char line[192];
        std::snprintf(line, sizeof line, pattern.c_str(), title.c_str());
        return std::string(line);
    }
    if (words.kind == screens::PlayerIconKind::latency)
        return spoken(screens::latency_grade_phrase(words.latency_grade));
    return {};
}

/// Appends one picture that speaks, under the window that holds it.
///
/// @param[in,out] windows the windows being filled
/// @param window_name the list or console
/// @param icon the picture
/// @param label the words
void append_spoken_image(
    std::vector<ExtensionWindow>& windows,
    const char* window_name,
    const oa::boneyards::Window* icon,
    const std::string& label,
    const char* description
) {
    if (window_name == nullptr || window_name[0] == '\0' || icon == nullptr ||
        icon->name == nullptr)
        return;
    oa::boneyards::DriverControlView shown{};
    shown.name = icon->name;
    shown.kind = oa::boneyards::DriverControlKind::image;
    shown.x = icon->x;
    shown.y = icon->y;
    shown.width = icon->width;
    shown.height = icon->height;
    shown.visible = true;
    shown.enabled = false;
    shown.text = "";
    shown.selected = -1;
    append_driver_control(&windows, window_name, shown);
    for (ExtensionWindow& window : windows) {
        if (window.name != window_name || window.controls.empty())
            continue;
        ExtensionControl& added = window.controls.back();
        if (added.name == icon->name) {
            added.kind = ExtensionControlKind::image;
            added.enabled = false;
            added.role_set = true;
            added.role = ExtensionControlRole::image;
            added.label = label;
            if (description != nullptr && description[0] != '\0')
                added.description = description;
        }
        break;
    }
}

/// Lists each shown rank and latency icon with the words for what it shows.
///
/// The pictures are not given captions, so they paint as they do today.
///
/// @param[in,out] windows the windows being filled
/// @param window a window to search; null searches nothing
void append_player_icons(std::vector<ExtensionWindow>& windows, oa::boneyards::Window* window) {
    if (window == nullptr || !oa::boneyards::window_is_shown(window))
        return;
    oa::boneyards::screens::PlayerIconWords words;
    if (oa::boneyards::screens::player_icon_words(window, &words)) {
        const std::string label = icon_label(words);
        const char* player_name = nullptr;
        const oa::boneyards::Window* row = window->parent;
        if (row != nullptr && row->ops != nullptr && row->ops->class_name != nullptr &&
            std::strcmp(row->ops->class_name, oa::boneyards::screens::kByPlayerRowClass) == 0)
            player_name = reinterpret_cast<const oa::boneyards::screens::ByPlayerRow*>(row)->name;
        if (!label.empty())
            append_spoken_image(windows, icon_window_name(window), window, label, player_name);
    }
    for (int32_t index = 0; index < window->children.count; ++index) {
        oa::boneyards::Window* child = window->children.items[index];
        if (child != nullptr)
            append_player_icons(windows, child);
    }
}

/// The old-column hotspot under a strip button. It stays enabled: the pointer
/// still lands there. A screen reader hears the strip button, so this one is
/// hidden.
///
/// @param name the control's name
/// @return true for SingleFormer, MultiFormer, IntroFormer and ExitFormer
bool menu_former(const std::string& name) {
    return name == "SingleFormer" || name == "MultiFormer" || name == "IntroFormer" ||
           name == "ExitFormer";
}

oa::boneyards::Window* find_named_on_desktop(const char* name) {
    oa::boneyards::console::StatusWidget* widget = boneyards_console_status_widget();
    if (widget == nullptr || name == nullptr)
        return nullptr;
    for (int32_t i = 0; i < widget->desktop.windows.count; ++i) {
        oa::boneyards::Window* top = widget->desktop.windows.items[i];
        if (top == nullptr)
            continue;
        oa::boneyards::Window* found = oa::boneyards::window_find_named(top, name, nullptr);
        if (found != nullptr)
            return found;
    }
    return nullptr;
}

/// The three player-pane tabs are one set. Their captions share the buttons'
/// rectangles, so those captions are hidden. The icons stay on Chat players.
void note_player_tabs(ExtensionWindow& window) {
    const char* names[3] = {"GeneralTabBtn", "LadderTabBtn", "AwardsTabBtn"};
    const char* captions[3] = {"General", "Ladder", "Awards"};
    int index[3] = {-1, -1, -1};
    for (std::size_t i = 0; i < window.controls.size(); ++i) {
        for (int tab = 0; tab < 3; ++tab) {
            if (window.controls[i].name == names[tab])
                index[tab] = static_cast<int>(i);
        }
    }
    if (index[0] < 0 || index[1] < 0 || index[2] < 0)
        return;
    int32_t left = window.controls[static_cast<std::size_t>(index[0])].x;
    int32_t top = window.controls[static_cast<std::size_t>(index[0])].y;
    int32_t right = left;
    int32_t bottom = top;
    for (int tab = 0; tab < 3; ++tab) {
        ExtensionControl& control = window.controls[static_cast<std::size_t>(index[tab])];
        const oa::boneyards::Window* live = find_named_on_desktop(names[tab]);
        control.place = tab + 1;
        control.set_size = 3;
        control.chosen = live != nullptr && live->access_chosen != 0;
        if (control.x < left)
            left = control.x;
        if (control.y < top)
            top = control.y;
        if (control.x + control.width > right)
            right = control.x + control.width;
        if (control.y + control.height > bottom)
            bottom = control.y + control.height;
    }
    for (ExtensionControl& control : window.controls) {
        if (control.kind != ExtensionControlKind::label)
            continue;
        bool caption = false;
        for (const char* name : captions) {
            if (control.name == name)
                caption = true;
        }
        if (!caption)
            continue;
        for (int tab = 0; tab < 3; ++tab) {
            const ExtensionControl& button = window.controls[static_cast<std::size_t>(index[tab])];
            if (control.x == button.x && control.y == button.y && control.width == button.width &&
                control.height == button.height)
                control.hidden = true;
        }
    }
    int earliest = index[0];
    if (index[1] < earliest)
        earliest = index[1];
    if (index[2] < earliest)
        earliest = index[2];
    ExtensionControl group;
    group.name = "PlayerTabs";
    group.kind = ExtensionControlKind::label;
    group.role_set = true;
    group.role = ExtensionControlRole::tab_list;
    group.enabled = false;
    group.visible = true;
    group.label = spoken("Player");
    group.x = left;
    group.y = top;
    group.width = right > left ? right - left : 0;
    group.height = bottom > top ? bottom - top : 0;
    window.controls.insert(window.controls.begin() + earliest, std::move(group));
}

/// Fills the words, roles and shortcuts of the controls just listed.
///
/// Window names and the text a control shows stay as they are. A planet's
/// facts line stays in `text`. Its label is its name and its value says who
/// holds it, how many battles it has, how many are forming and whether it
/// is the chosen planet. One battle uses the singular line. An id alias and
/// a former stay enabled and are marked hidden.
///
/// @param[in,out] windows the consoles and their controls
/// @param probe the open main frame; null leaves no planet chosen
void fill_access(
    std::vector<ExtensionWindow>& windows, const oa::boneyards::console::MainFrameProbe* probe
) {
    namespace console = oa::boneyards::console;
    const uint32_t chosen_area = probe != nullptr ? probe->selected_area_id : 0;
    const std::string battles_line = spoken_pattern(console::k_planet_value);
    const std::string battle_line = spoken_pattern(console::k_planet_battle);
    const std::string battles_chosen_line = spoken_pattern(console::k_planet_value_chosen);
    const std::string battle_chosen_line = spoken_pattern(console::k_planet_battle_chosen);
    const console::PlanetValuePatterns patterns{
        battles_line.c_str(),
        battle_line.c_str(),
        battles_chosen_line.c_str(),
        battle_chosen_line.c_str(),
    };
    for (ExtensionWindow& window : windows) {
        for (ExtensionControl& control : window.controls) {
            for (const SpokenButton& button : k_spoken_buttons) {
                if (control.name != button.name)
                    continue;
                control.label = spoken(button.label);
                if (button.shortcut[0] != '\0')
                    control.shortcut = button.shortcut;
                if (button.tab) {
                    control.role_set = true;
                    control.role = ExtensionControlRole::tab;
                }
                if (menu_former(control.name))
                    control.hidden = true;
                break;
            }
            if ((control.kind == ExtensionControlKind::check_box ||
                 control.kind == ExtensionControlKind::button) &&
                (control.name == "Arm" || control.name == "Core"))
                control.label = spoken(control.name.c_str());
            int planet_id = 0;
            char owner = '\0';
            if (console::planet_parse_facts(
                    control.text.c_str(), &planet_id, &owner, nullptr, nullptr
                )) {
                const bool chosen =
                    chosen_area != 0 && static_cast<uint32_t>(planet_id) == chosen_area;
                const std::string planet_name = planet_label_of(window, control);
                const std::string owner_word = spoken(console::planet_owner_word(owner));
                char label[64];
                char value[192];
                if (console::planet_access_words(
                        label,
                        sizeof label,
                        value,
                        sizeof value,
                        control.text.c_str(),
                        planet_name.c_str(),
                        chosen,
                        patterns,
                        owner_word.c_str()
                    )) {
                    control.label = label;
                    control.value_text = value;
                    control.chosen = chosen;
                    // The driver's id alias (1801, and the off-viewer id label)
                    // stays enabled, at its rectangle. The planet's own name is
                    // the one a player hears.
                    if (digits_only(control.name))
                        control.hidden = true;
                }
            }
            int pick = 0;
            if (indexed_name(control.name, "Pick", &pick)) {
                const ExtensionControl* type_label =
                    find_named(window, std::string("Label") + std::to_string(pick));
                if (type_label != nullptr && !type_label->text.empty())
                    control.label = type_label->text;
            }
            int row = 0;
            if (indexed_name(control.name, "row_", &row)) {
                const ExtensionControl* kind =
                    find_named(window, std::string("kind_") + std::to_string(row));
                if (kind != nullptr && !kind->text.empty())
                    control.label = kind->text;
            }
            int hit = 0;
            if (indexed_name(control.name, "rhit", &hit)) {
                std::string result;
                const char* parts[] = {"rname", "rwhere", "rstatus"};
                for (const char* part : parts) {
                    const ExtensionControl* piece =
                        find_named(window, std::string(part) + std::to_string(hit));
                    if (piece == nullptr || piece->text.empty())
                        continue;
                    if (!result.empty())
                        result += ", ";
                    result += piece->text;
                }
                if (!result.empty())
                    control.label = std::move(result);
            }
        }
        constexpr int k_max_win_icons = 8;
        ExtensionControl* icons[k_max_win_icons]{};
        int32_t tops[k_max_win_icons]{};
        int icon_count = 0;
        for (ExtensionControl& control : window.controls) {
            if (control.name != "SmallArmIcon" || icon_count == k_max_win_icons)
                continue;
            icons[icon_count] = &control;
            tops[icon_count] = control.y;
            ++icon_count;
        }
        const char* words[k_max_win_icons]{};
        console::summary_win_icon_labels(tops, static_cast<std::size_t>(icon_count), words);
        for (int icon = 0; icon < icon_count; ++icon) {
            icons[icon]->label = spoken(words[icon]);
            icons[icon]->role_set = true;
            icons[icon]->role = ExtensionControlRole::image;
        }
        for (std::size_t index = 0; index < window.controls.size(); ++index) {
            if (window.controls[index].name != "RadioGroup")
                continue;
            std::string selected;
            std::size_t next = index + 1;
            while (next < window.controls.size() &&
                   window.controls[next].kind == ExtensionControlKind::check_box) {
                ExtensionControl& radio = window.controls[next];
                radio.role_set = true;
                radio.role = ExtensionControlRole::radio;
                radio.chosen = radio.checked;
                if (radio.checked)
                    selected = radio.text.empty() ? radio.name : radio.text;
                ++next;
            }
            std::string title;
            if (next < window.controls.size() &&
                window.controls[next].kind == ExtensionControlKind::label) {
                const ExtensionControl& caption = window.controls[next];
                title = caption.text.empty() ? caption.name : caption.text;
            }
            ExtensionControl& group = window.controls[index];
            group.role_set = true;
            group.role = ExtensionControlRole::radio_group;
            if (!title.empty())
                group.label = spoken(title.c_str());
            if (!selected.empty())
                group.value_text = spoken(selected.c_str());
        }
        note_player_tabs(window);
    }
}

/// States the rectangle where the strip's art takes the press.
///
/// The panel is drawn across the bottom of the main menu. A screen button
/// that lies wholly inside it is not seen and takes no click, so the
/// listing hides it. Button mode and none mode draw no such panel.
///
/// @param[in,out] windows the consoles being listed
void state_strip_press(std::vector<ExtensionWindow>& windows) {
    namespace console = oa::boneyards::console;
    for (ExtensionWindow& window : windows) {
        if (window.name != "MainMenuOverlay")
            continue;
        window.press_x = console::overlay_layout::x;
        window.press_y = console::overlay_layout::y;
        window.press_width = console::overlay_layout::width;
        window.press_height = console::overlay_layout::height;
    }
}

/// Keeps the main menu's strip in the listing to what the menu shows.
///
/// With the strip's rows not shown (button and none modes) its buttons take
/// no click, so they are left out. In button mode only ENTER is listed,
/// where it stands once the plate's place is applied. Credits is the menu's
/// own control, at the place it is drawn, and is not listed here. While the
/// strip is drawn, its window states the rectangle where that art takes the
/// press.
///
/// @param[in,out] windows the consoles being listed
void list_menu_entry_as_shown(std::vector<ExtensionWindow>& windows) {
    namespace console = oa::boneyards::console;
    console::MenuEntryMode mode = console::MenuEntryMode::strip;
    oa::ui::frontend_renderer::SourceRect place{};
    if (boneyards_console_up() ||
        !boneyards_console_menu_entry(&mode, &place.x, &place.y, &place.width, &place.height))
        return;
    if (mode == console::MenuEntryMode::strip) {
        state_strip_press(windows);
        return;
    }
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

/// Names the region's first stop when the toolkit focuses none.
///
/// The report only changes. The toolkit's own focus, and the frame it draws,
/// stay as they are. A focused control that is hidden still counts as the
/// toolkit's focus. The first stop is the visible, enabled control with area
/// that is not a label or a picture, earliest in reading order.
///
/// @param[in,out] windows the consoles being listed
void note_region_first_stop(std::vector<ExtensionWindow>& windows) {
    if (!boneyards_console_up())
        return;
    for (const ExtensionWindow& window : windows)
        for (const ExtensionControl& control : window.controls)
            if (control.focused)
                return;
    ExtensionControl* stop = nullptr;
    int64_t best = 0;
    for (ExtensionWindow& window : windows) {
        for (ExtensionControl& control : window.controls) {
            if (!control.visible || !control.enabled || control.hidden)
                continue;
            if (control.kind == ExtensionControlKind::label ||
                control.kind == ExtensionControlKind::image)
                continue;
            if (control.width <= 0 || control.height <= 0)
                continue;
            const int64_t score =
                static_cast<int64_t>(control.y) * 5000 + static_cast<int64_t>(control.x);
            if (stop == nullptr || score < best) {
                stop = &control;
                best = score;
            }
        }
    }
    if (stop != nullptr)
        stop->focused = true;
}

/// Lists the consoles the Boneyards is showing (ExtensionWindowSource).
///
/// The ticker reads its whole line. While the map is locked (the green layer
/// slides or covers it, or a console is open), a press on a planet selects
/// nothing, so each planet on the viewer is listed as not taking a click.
/// Picture buttons, icons and planets carry the words a player hears. Their
/// window names, and the facts line a planet shows, stay as they are.
/// On a main menu without the strip the plate's ENTER is listed. Credits is
/// the menu's own control there, not a second button under the plate.
/// While the strip is drawn, its window states where its art takes the press.
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
        if (widget != nullptr) {
            for (int32_t index = 0; index < widget->desktop.windows.count; ++index)
                append_player_icons(windows, widget->desktop.windows.items[index]);
        }
        list_menu_entry_as_shown(windows);
        oa::boneyards::console::MainFrameProbe probe{};
        const bool probed = oa::boneyards::console::boneyards_status_widget_probe(widget, &probe);
        fill_access(windows, probed ? &probe : nullptr);
        if (probed) {
            for (ExtensionWindow& window : windows) {
                const bool planets = window.name == "MapPanel";
                for (ExtensionControl& control : window.controls) {
                    if (control.name == "Ticker")
                        control.text = probe.ticker_text;
                    if (planets && probe.map_locked) {
                        control.enabled = false;
                        control.hidden = true;
                    }
                }
            }
        }
        note_region_first_stop(windows);
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
/// cannot start a game says so instead. An extension that has taken the game
/// over keeps the main menu too. Otherwise the strip opens multiplayer.
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
            register_boneyards_launch_check(
                registry, *boneyards_options().boneyards_launch, boneyards_options().launch_for_fun
            );
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
            if (boneyards_launch_notice_pending()) {
                snapshot_presented_stage(
                    runtime, oa::boneyards::screens::after_battle_picture_name()
                );
                boneyards_launch_notice_taken();
            }
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
    // Share… on the For Fun prompt leaves these screens and opens that
    // battle's recording in the Library, with the share question. The
    // prompt only records the choice; this frame takes it.
    if (oa::boneyards::screens::take_after_battle_share()) {
        boneyards_console_exit_to_menu();
        queue_battle_replay_share(runtime);
    }
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
    oa::boneyards::screens::mark_after_battle_played();
}

/// Notes a match that finished, was left or was torn down, which the Boneyards box follows (Extension::match_event).
///
/// @param context Extension context (unused).
/// @param runtime The running app.
/// @param event What happened to the match.
void match_event(void* /*context*/, Runtime& runtime, MatchEvent event) {
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
    // torn_down also runs when a new battle's setup destroys the previous
    // match, before that battle starts. The recording is sampled only on the
    // way out.
    if (event == MatchEvent::finished || event == MatchEvent::left)
        oa::boneyards::screens::mark_after_battle_recorded(battle_has_recording(runtime));
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
    hooks.recorded_game = app::boneyards_recorded_game;
    oa::app::netgame::extension_api::set_hooks(hooks);
    oa::app::register_expansion_overlay();
    oa::app::register_boneyards_replay_kind();
}
