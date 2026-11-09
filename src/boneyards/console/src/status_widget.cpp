// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Boneyards system object on the main-menu path: GUI start-up, showing and
// releasing the overlay, message-box coupling and the "Enter Boneyards" entry.
#include "oa/boneyards/console/status_widget.hpp"

#include "oa/boneyards/font_atlases.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/chat.hpp"
#include "oa/boneyards/screens/galaxy.hpp"
#include "oa/boneyards/utf8.hpp"

#include "oa/platform/system.hpp"
#include "oa/present/colour_vision/colour_vision.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "ad_banner.hpp"
#include "battle_console.hpp"
#include "console_layer.hpp"
#include "console_state.hpp"
#include "oa/boneyards/console/feature_consoles.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/release.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

constexpr int32_t k_screen_width = 640;
constexpr int32_t k_screen_height = 480;
// Colour-vision mode the open hat fonts were tinted for. Not a mode until the fonts exist.
uint8_t g_role_font_mode = 0xFF;
Handle g_muzzle_font = k_no_handle;

std::span<const uint8_t> palette_bytes(const StatusWidget* widget) {
    if (widget == nullptr || widget->config.palette == nullptr)
        return {};
    return std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(widget->config.palette->entries), sizeof(oa::Palette)
    );
}

void publish_role_fonts(StatusWidget* widget) {
    if (widget == nullptr || widget->navigation == nullptr)
        return;
    screens::ChatSession& session = widget->navigation->session;
    const screens::HatFonts& fonts = widget->system.hat_fonts;
    session.system_font = fonts.small_green;
    session.rank_0x15_font = fonts.small_orange;
    session.arm_font = fonts.small_blue;
    session.core_font = fonts.small_red;
}

void release_muzzle_font() {
    if (g_muzzle_font >= 0)
        font_release(g_muzzle_font);
    g_muzzle_font = k_no_handle;
    screens::boneyards_muzzle_font(k_no_handle);
}

// Tints the four chat roles, and a muzzled name while a mode is on. Off uses today's indices and
// the dim font for a muzzled name.
void tint_role_fonts(StatusWidget* widget) {
    if (widget == nullptr || widget->toolkit == nullptr)
        return;
    auto& fonts = widget->system.hat_fonts;
    if (fonts.small < 0)
        return;
    const screens::GalaxyChatColours colours = screens::galaxy_chat_colours(palette_bytes(widget));
    const Handle previous[] = {
        fonts.small_green, fonts.small_orange, fonts.small_blue, fonts.small_red
    };
    fonts.small_green =
        toolkit_font_tint(widget->toolkit, fonts.small, static_cast<uint8_t>(colours.system));
    fonts.small_orange =
        toolkit_font_tint(widget->toolkit, fonts.small, static_cast<uint8_t>(colours.newsbot));
    fonts.small_blue =
        toolkit_font_tint(widget->toolkit, fonts.small, static_cast<uint8_t>(colours.arm));
    fonts.small_red =
        toolkit_font_tint(widget->toolkit, fonts.small, static_cast<uint8_t>(colours.core));
    for (const Handle old : previous) {
        if (old >= 0)
            font_release(old);
    }
    release_muzzle_font();
    if (present::colour_vision::active_mode() != present::colour_vision::Mode::off) {
        const int32_t ink = screens::muzzle_name_index(palette_bytes(widget));
        g_muzzle_font = toolkit_font_tint(widget->toolkit, fonts.small, static_cast<uint8_t>(ink));
        screens::boneyards_muzzle_font(g_muzzle_font);
    }
    g_role_font_mode = static_cast<uint8_t>(present::colour_vision::active_mode());
    publish_role_fonts(widget);
}

void refresh_role_fonts(StatusWidget* widget) {
    if (widget == nullptr || !widget->system.fonts_loaded)
        return;
    if (g_role_font_mode == static_cast<uint8_t>(present::colour_vision::active_mode()))
        return;
    tint_role_fonts(widget);
}

const char* const k_cursor_file = "cursors.gaf";
const char* const k_cursor_sequence = "cursornormal";
// Key values of typed characters.
constexpr int32_t k_first_character = 0x20;
constexpr int32_t k_last_character = 0xFF;
// The planet summary's picture area of the Main Frame background right of
// its name and owner labels, and the fill its draw clears it to.
constexpr int32_t k_summary_picture_left = 0x24d;
constexpr int32_t k_summary_picture_right = 0x26d;
constexpr int32_t k_summary_picture_top = 0x7e;
constexpr int32_t k_summary_picture_bottom = 0x9c;
constexpr uint8_t k_summary_picture_fill = 0xaf;
// How often the idle work's check runs while a launched battle is under way.
constexpr uint32_t k_check_period_ms = 1000;

uint8_t* read_for_toolkit(void* user, const char* path, size_t* size) {
    const FileReader& files = static_cast<StatusWidget*>(user)->config.files;
    std::vector<uint8_t> bytes;
    if (files.read == nullptr || !files.read(files.user, path, &bytes))
        return nullptr;
    auto* out = static_cast<uint8_t*>(std::malloc(bytes.size() + 1));
    if (out == nullptr)
        return nullptr;
    // An empty file has no data pointer. Nothing is copied.
    if (!bytes.empty())
        std::memcpy(out, bytes.data(), bytes.size());
    *size = bytes.size();
    return out;
}

// Plays a toolkit sound file through the host hook the rest of the overlay
// uses. The toolkit names a file under sounds\; the host looks the file up
// by its name. The volume is the overlay's own interface level.
void play_toolkit_sound(void* user, const char* path, int32_t /*volume*/) {
    auto* widget = static_cast<StatusWidget*>(user);
    const MainMenuHost& host = widget->config.host;
    if (host.play_sound == nullptr || path == nullptr || path[0] == '\0')
        return;
    const char* name = path;
    for (const char* scan = path; *scan != '\0'; ++scan) {
        if (*scan == '\\' || *scan == '/')
            name = scan + 1;
    }
    if (name[0] != '\0')
        host.play_sound(host.user, name);
}

// Full path of the atlas beside the bundled fonts. OA_FONT_ATLASES_DIR, when
// set, names a directory that holds the file instead.
std::string g_atlas_path;
bool g_atlas_reported = false;

const char* k_analogue_atlas_name = "boneyards-analogues.bin";

// Page fonts, opened with the Boneyards window and closed after its pages are gone.
void (*g_open_window_fonts)(StatusWidget*) = nullptr;
void (*g_close_window_fonts)(StatusWidget*) = nullptr;

FontAtlas* open_atlas_file(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::is_regular_file(path, error)) {
        return nullptr;
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(in), {});
    if (bytes.empty()) {
        return nullptr;
    }
    return font_atlas_open(bytes.data(), bytes.size());
}

// The analogue atlas shipped in the game's fonts folder. OA_FONT_ATLASES_DIR
// overrides that path. A missing file is reported once; the faces' own glyphs
// still draw.
FontAtlas* open_analogue_atlas() {
    namespace fs = std::filesystem;
    fs::path path;
    if (const auto dir = oa::platform::environment_value("OA_FONT_ATLASES_DIR");
        dir && !dir->empty()) {
        path = fs::path(*dir) / k_analogue_atlas_name;
    } else if (!g_atlas_path.empty()) {
        path = g_atlas_path;
    }
    if (!path.empty()) {
        if (FontAtlas* atlas = open_atlas_file(path); atlas != nullptr) {
            return atlas;
        }
    }
    if (!g_atlas_reported) {
        g_atlas_reported = true;
        std::fprintf(
            stderr,
            "analogue atlas: the fonts folder has no %s, so characters a face does not contain "
            "are not drawn\n",
            k_analogue_atlas_name
        );
    }
    return nullptr;
}

// Text, fonts, colours, key state, sounds, carets and the cursor come from a
// toolkit whose drawing is redirected into the frame being drawn; images, the
// cursor's frames among them, stay with the console store.
void attach_toolkit(StatusWidget* widget) {
    if (widget->config.palette == nullptr) {
        screens::boneyards_use_palette(nullptr, 0);
        return;
    }
    screens::boneyards_use_palette(
        reinterpret_cast<const uint8_t*>(widget->config.palette->entries), sizeof(oa::Palette)
    );
    ToolkitConfig config{};
    config.width = k_screen_width;
    config.height = k_screen_height;
    config.palette = widget->config.palette;
    config.files = ToolkitFiles{widget, read_for_toolkit};
    config.audio = ToolkitAudio{widget, play_toolkit_sound};
    // The menu defers the screen models and, with them, the toolkit tables
    // and the default font. A test widget leaves both ready at creation.
    config.defer_tables = widget->config.defer_models;
    widget->toolkit = toolkit_create(&config);
    const Services shared = toolkit_services(widget->toolkit);
    widget->services.texts = shared.texts;
    widget->services.fonts = shared.fonts;
    widget->services.input = shared.input;
    widget->services.colors = shared.colors;
    widget->services.cursors = shared.cursors;
    widget->services.sounds = shared.sounds;
    widget->services.draw = shared.draw;
    toolkit_set_cursor_images(widget->toolkit, &widget->image_service);
}

/// Opens the page fonts, the analogue atlas and the war-map graphics.
///
/// The main-menu strip does not need them. A window that opens again before
/// the previous pages are destroyed keeps the fonts those pages still close.
///
/// @param[in,out] widget status widget
void ensure_window_resources(StatusWidget* widget) {
    widget->window_fonts_pending = false;
    if (g_open_window_fonts != nullptr)
        g_open_window_fonts(widget);
    if (widget->analogues == nullptr && widget->toolkit != nullptr) {
        widget->analogues = open_analogue_atlas();
        toolkit_set_font_atlas(widget->toolkit, widget->analogues);
    }
    if (widget->galaxy_graphics == nullptr) {
        widget->galaxy_graphics = std::make_unique<GalaxyGraphics>();
        if (!galaxy_load_graphics(*widget->galaxy_graphics, widget->config.files))
            widget->galaxy_graphics.reset();
        else
            widget->galaxy_graphics->palette = widget->config.palette;
    } else {
        widget->galaxy_graphics->palette = widget->config.palette;
    }
}

/// Asks for the window's fonts, atlas and war-map graphics to close.
///
/// They stay until the window's pages have been destroyed: a page still holds
/// the face it closes with, and the window may still draw.
///
/// @param[in,out] widget status widget
void release_window_graphics(StatusWidget* widget) {
    widget->window_fonts_pending = true;
}

/// Closes the page fonts, the atlas and the war-map graphics once the window is gone.
///
/// @param[in,out] widget status widget
void close_window_fonts(StatusWidget* widget) {
    if (!widget->window_fonts_pending || widget->system.main_frame != nullptr)
        return;
    widget->window_fonts_pending = false;
    if (widget->toolkit != nullptr)
        toolkit_set_font_atlas(widget->toolkit, nullptr);
    font_atlas_close(widget->analogues);
    widget->analogues = nullptr;
    widget->galaxy_graphics.reset();
    if (g_close_window_fonts != nullptr)
        g_close_window_fonts(widget);
}

void call_state(const MainMenuHost& host, uint8_t state) {
    if (host.set_frontend_state != nullptr)
        host.set_frontend_state(host.user, state);
}

// Client events: the login dialog, notices and war-map feeds are the
// screens', system alerts the Main Frame's and battle lines the host's; the
// session level and matchmaking_enabled are mirrored into the system object,
// and the Main Frame's activity light, which runs from LOGIN, stops when the
// login attempt ends or the dossier arrives, and after a hotspot press when
// the census of a planet on the map does.
StatusWidget* event_widget(void* user) {
    return static_cast<StatusWidget*>(user);
}

// The locator kept on the Main Frame, shown or hidden. Null when that frame
// has not been built.
Window* open_locator(StatusWidget* widget) {
    MainFrame* frame = widget != nullptr ? widget->system.main_frame : nullptr;
    if (frame == nullptr)
        return nullptr;
    return window_find_named(&frame->base, k_locator_console_name, nullptr);
}

/// Stops the activity light of the Main Frame the Boneyards screens belong to.
///
/// @param widget status widget
void stop_main_frame_light(const StatusWidget* widget) {
    if (MainFrame* frame = widget->system.main_frame; frame != nullptr)
        activity_light_stop(frame->activity_light);
}

/// Starts the activity light of the Main Frame the Boneyards screens belong to.
///
/// @param widget status widget
void start_main_frame_light(const StatusWidget* widget) {
    if (MainFrame* frame = widget->system.main_frame; frame != nullptr)
        activity_light_start(frame->activity_light);
}

// The screens' activity light is the Main Frame's.
void screens_activity_light(void* user, bool running) {
    const auto* widget = static_cast<const StatusWidget*>(user);
    if (running)
        start_main_frame_light(widget);
    else
        stop_main_frame_light(widget);
}

void on_login_status(void* user, const char* text) {
    screens::boneyards_on_login_status(event_widget(user)->navigation, text);
}

void on_login_reset(void* user) {
    StatusWidget* widget = event_widget(user);
    stop_main_frame_light(widget);
    // A server transfer logs in from this reset, before the login dialog sees it.
    if (Window* locator = open_locator(widget); locator != nullptr)
        locator_console_on_login_reset(locator);
    screens::boneyards_on_login_reset(widget->navigation);
}

void on_session(void* user, const SessionRecord* session) {
    StatusWidget* widget = event_widget(user);
    ConsoleSystem& system = widget->system;
    system.matchmaking_enabled = session->matchmaking_enabled;
    oa::boneyards::utf8::copy(
        system.user_name,
        sizeof system.user_name,
        session->user_name,
        oa::boneyards::kPlayerNameCharacters
    );
    const screens::LoginDialog& login = widget->navigation->login;
    const screens::SettingsStore& settings = widget->config.settings;
    if (login.state == screens::LoginState::connecting &&
        session->level == SessionLevel::logged_in) {
        // The name typed, until the dossier replaces it with the server's.
        if (settings.save_string != nullptr)
            settings.save_string(settings.user, "Username", login.name);
    }
    if (session->level == SessionLevel::dossier_loaded &&
        system.connection_level != static_cast<int32_t>(SessionLevel::dossier_loaded)) {
        stop_main_frame_light(widget);
        // The session bit is the dossier status: 1 for Admin or Beta Tester.
        // A login that never reaches the dossier stores nothing. The login
        // name kept is the one the dossier returned.
        if (login.state == screens::LoginState::connecting) {
            if (settings.save_string != nullptr && session->user_name[0] != '\0')
                settings.save_string(settings.user, "Username", session->user_name);
            if (settings.save_number != nullptr) {
                const int32_t status =
                    session->matchmaking_enabled != 0 ? kAccountStatusAdmin : kAccountStatusActive;
                settings.save_number(
                    settings.user, kBetaLoginUxKey, beta_login_ux_for_status(status)
                );
            }
        }
    }
    screens::boneyards_on_session(widget->navigation, session);
    system.connection_level = static_cast<int32_t>(session->level);
    if (session->level == SessionLevel::offline)
        system.pump_enabled = 0;
}

/// Stores a client notice for the Main Frame's next cycle.
///
/// That cycle opens the notice box. Dismissing the box leaves for the main
/// menu. With no Main Frame the notice opens at once.
///
/// @param user the StatusWidget
/// @param text notice text; kept until the cycle that opens the box
void on_notice(void* user, const char* text) {
    StatusWidget* widget = event_widget(user);
    if (MainFrame* frame = widget->system.main_frame; frame != nullptr)
        frame->pending_notice = text;
    else
        screens::boneyards_on_notice(widget->navigation, text);
}

/// Hands a system alert to the open Main Frame.
///
/// @param user the StatusWidget
/// @param alert system alert
void on_system_alert(void* user, const AlertReport* alert) {
    if (MainFrame* frame = event_widget(user)->system.main_frame; frame != nullptr)
        main_frame_on_alert(frame, alert);
}

bool on_battle_active(void* user) {
    return event_widget(user)->system.chat_console_pending != 0;
}

void on_battle_message(void* user, const char* text) {
    const MainMenuHost& host = event_widget(user)->config.host;
    if (host.battle_message != nullptr)
        host.battle_message(host.user, text);
}

void on_allegiances(void* user, const char* allegiances) {
    screens::boneyards_on_allegiances(event_widget(user)->navigation, allegiances);
}

void on_activity(void* user, const char* digits) {
    screens::boneyards_on_activity(event_widget(user)->navigation, digits);
}

/// Passes a census to the navigation state and, as in 3.1c, to the open Recon console too.
///
/// @param user the status widget
/// @param report the census
void on_census(void* user, const CensusReport* report) {
    StatusWidget* widget = event_widget(user);
    if (screens::boneyards_on_census(widget->navigation, report))
        stop_main_frame_light(widget);
    MainFrame* frame = widget->system.main_frame;
    if (frame != nullptr && frame->console_screen == console_screen::recon &&
        frame->active_screen != nullptr && widget->config.screens.create == nullptr)
        recon_console_on_census(frame->active_screen, report);
}

// A frequency search collects these rows. Otherwise they are the Chat
// Frequencies console's, which asks only for the planet the player is on: a
// row of another planet is the end of a search the Locator left, and is
// dropped.
void on_room_listed(
    void* user,
    int32_t change,
    uint32_t area_id,
    uint32_t room_id,
    const char* name,
    const char* description,
    bool needs_password,
    int32_t users,
    int32_t type,
    const char* tag,
    bool last,
    bool global_known,
    int32_t global
) {
    StatusWidget* widget = event_widget(user);
    if (Window* locator = open_locator(widget);
        locator != nullptr && locator_console_takes_rooms(locator)) {
        locator_console_on_room(
            locator,
            change,
            area_id,
            room_id,
            name,
            needs_password,
            users,
            type,
            last,
            global_known,
            global
        );
        return;
    }
    if (area_id != widget->navigation->session.area_id)
        return;
    const screens::RoomListing room{room_id, name, description, needs_password, users, type, tag};
    screens::boneyards_on_room_listed(widget->navigation, change, &room, last);
}

void on_room_updated(
    void* user,
    int32_t change,
    uint32_t,
    uint32_t room_id,
    const char* name,
    const char* description,
    bool needs_password,
    int32_t users,
    int32_t type,
    const char* tag
) {
    const screens::RoomListing room{room_id, name, description, needs_password, users, type, tag};
    screens::boneyards_on_room_updated(event_widget(user)->navigation, change, &room);
}

// The chat opens and closes its panes as a frequency is entered or created.
void sync_chat_panes() {
    if (ChatView* chat = console_open_chat(); chat != nullptr)
        chat_panes_sync(chat);
}

void on_room_entered(void* user, int32_t result) {
    screens::boneyards_on_room_entered(event_widget(user)->navigation, result);
    sync_chat_panes();
}

void on_room_member(
    void* user,
    int32_t change,
    uint32_t user_id,
    const char* name,
    int32_t latency,
    int32_t icon,
    int32_t rank,
    const char* allegiance
) {
    screens::boneyards_on_room_member(
        event_widget(user)->navigation, change, user_id, name, latency, icon, rank, allegiance
    );
}

void on_chat_message(void* user, bool from_server, uint32_t user_id, const char* text) {
    screens::boneyards_on_chat_message(event_widget(user)->navigation, user_id, text, from_server);
}

void on_room_left(void* user, bool failed) {
    screens::boneyards_on_room_left(event_widget(user)->navigation, failed);
}

void on_room_created(void* user, uint32_t area_id, uint32_t room_id, const char* name) {
    screens::boneyards_on_room_created(event_widget(user)->navigation, area_id, room_id, name);
    sync_chat_panes();
}

// A banner fetch is claimed by the strip. Anything else goes to the contents console.
void on_file_arrived(void* user, const char* url, const char* path, int32_t status) {
    const MainFrame* frame = event_widget(user)->system.main_frame;
    if (frame != nullptr && frame->ad_banner != nullptr &&
        ad_banner_note_download(frame->ad_banner, url, path, status))
        return;
    contents_console_file_arrived(
        frame != nullptr ? frame->contents_console : nullptr, url, path, status
    );
}

void on_battles_done(void* user, bool failed) {
    screens::boneyards_on_battles_done(event_widget(user)->navigation, failed);
}

void on_battle_info(
    void* user,
    int32_t change,
    const char* game_id,
    const char* name,
    int32_t type,
    int32_t forming,
    int32_t host_code
) {
    screens::boneyards_on_battle_info(
        event_widget(user)->navigation, change, game_id, name, type, forming, host_code
    );
}

void on_intel(void* user, const char* game_id, const char* text) {
    screens::boneyards_on_intel(event_widget(user)->navigation, game_id, text);
}

void on_battle_intel(void* user, const BattleIntel* intel) {
    StatusWidget* widget = event_widget(user);
    Window* locator = open_locator(widget);
    // The selected battle's intel updates the locator's detail. The battle
    // list hears it too.
    if (locator != nullptr && locator_console_takes_intel(locator) && intel != nullptr) {
        locator_console_on_intel(
            locator, intel->game_id, intel->user_name, intel->skill, intel->failed
        );
    }
    screens::boneyards_on_battle_intel(widget->navigation, intel);
}

void on_battle_report(void* user, const BattleReport* report) {
    screens::boneyards_on_battle_report(event_widget(user)->navigation, report);
}

/// Copies a game id into a system record field, keeping at most 36 characters.
///
/// @param[out] field field receiving the id
/// @param id game id; null stores ""
void copy_game_id(char (&field)[k_game_id_capacity], const char* id) {
    std::snprintf(field, sizeof field, "%s", id != nullptr ? id : "");
}

/// Enters the battle a launch reply started, as the Boneyards does after the launch.
///
/// The debriefing starts out as the current area's, with no room or game;
/// the battle counts as launched and just started, the Main Frame hides and
/// the frontend goes on in the connection selection with the initialize
/// signal, which a frontend pass then runs.
///
/// @param[in,out] widget status widget
void console_enter_post_join(StatusWidget* widget) {
    ConsoleSystem& system = widget->system;
    screens::ChatSession& session = widget->navigation->session;
    session.debrief_area_id = session.area_id;
    session.debrief_room_id = 0;
    system.debriefing_game_id[0] = '\0';
    system.chat_console_pending = 1;
    session.debriefing_pending = true;
    system.just_launched = 1;
    if (system.main_frame != nullptr)
        system.main_frame->base.visible = 0;
    const MainMenuHost& host = widget->config.host;
    call_state(host, frontend_value::state_connection_selection);
    if (host.set_signal != nullptr)
        host.set_signal(host.user, frontend_value::signal_initialize);
    if (host.run_frontend != nullptr)
        host.run_frontend(host.user);
}

/// Handles the launch_game reply to a battle hosted or joined.
///
/// Result 0 keeps the game id and the planet the reply named, hands the
/// launch to the host, stops the activity light, closes the battle console
/// (which leaves the planet's battles) and enters the battle, unless the
/// host refuses this simulation, which opens its refusal and does not enter.
/// Any other result opens a message box saying why the battle was refused.
///
/// @param user the StatusWidget
/// @param launch the reply
void on_launch(void* user, const LaunchReport* launch) {
    StatusWidget* widget = event_widget(user);
    if (launch->result != 0) {
        const auto index = static_cast<std::size_t>(launch->result) - 1U;
        const char* text = launch->result > 0 && index < std::size(launch_message::refused)
                               ? launch_message::refused[index]
                               : launch_message::unknown;
        if (widget->system.gui_ready != 0)
            message_box_create(text, message_box_buttons::ok, nullptr, nullptr, nullptr);
        return;
    }
    const MainMenuHost& host = widget->config.host;
    if (host.launch_permitted != nullptr) {
        const char* refusal = nullptr;
        if (!host.launch_permitted(host.user, &refusal)) {
            if (widget->config.service.leave_battles != nullptr)
                widget->config.service.leave_battles(widget->config.service.user, launch->area_id);
            if (widget->system.gui_ready != 0 && refusal != nullptr && refusal[0] != '\0')
                message_box_create(refusal, message_box_buttons::ok, nullptr, nullptr, nullptr);
            return;
        }
    }
    ConsoleSystem& system = widget->system;
    copy_game_id(system.launched_game_id, launch->game_id);
    system.launched_area_id = launch->area_id;
    // The galactic map as it stands names the planet and its sector; they are
    // kept with the game id so that the Main Frame selecting another planet
    // later does not change them.
    system.launched_planet[0] = '\0';
    system.launched_sector[0] = '\0';
    if (widget->navigation != nullptr) {
        const screens::GalaxyView& galaxy = widget->navigation->galaxy;
        std::snprintf(
            system.launched_sector, sizeof system.launched_sector, "%s", galaxy.sector_name
        );
        for (std::size_t index = 0; index < galaxy.planet_count; ++index) {
            if (static_cast<uint32_t>(galaxy.planets[index].id) != launch->area_id)
                continue;
            std::snprintf(
                system.launched_planet,
                sizeof system.launched_planet,
                "%s",
                galaxy.planets[index].name
            );
            break;
        }
    }
    if (host.battle_launched != nullptr)
        host.battle_launched(host.user, launch, system.user_name);
    stop_main_frame_light(widget);
    if (MainFrame* frame = system.main_frame;
        frame != nullptr && frame->console_screen == console_screen::battle &&
        frame->active_screen != nullptr && frame->screen_close != nullptr)
        frame->screen_close(frame->active_screen);
    console_enter_post_join(widget);
}

/// Records the room the launched battle's players are debriefed in, from the first debriefing only.
///
/// @param user the StatusWidget
/// @param area_id the debriefing's area
/// @param room_id the debriefing room
/// @param first whether it is the first debriefing of the battle
void on_debriefing(void* user, uint32_t area_id, uint32_t room_id, bool first) {
    if (!first)
        return;
    StatusWidget* widget = event_widget(user);
    screens::ChatSession& session = widget->navigation->session;
    session.debrief_area_id = area_id;
    session.debrief_room_id = room_id;
    copy_game_id(widget->system.debriefing_game_id, widget->system.launched_game_id);
}

// The text a label shows in its current state, "" without one.
void read_label(const StatusWidget* widget, const Window* label, char* text, std::size_t capacity) {
    const TextService* texts = widget->services.texts;
    const int32_t current = label != nullptr ? label->states.current : -1;
    const Handle handle =
        current >= 0 && current < label->states.count ? label->states.texts[current] : k_no_handle;
    const char* value = texts != nullptr && texts->get != nullptr && handle >= 0
                            ? texts->get(texts->self, handle)
                            : nullptr;
    std::snprintf(text, capacity, "%s", value != nullptr ? value : "");
}

void listen_to_service(StatusWidget* widget) {
    ServiceEvents events{};
    events.user = widget;
    events.login_status = on_login_status;
    events.login_reset = on_login_reset;
    events.session_changed = on_session;
    events.notice = on_notice;
    events.system_alert = on_system_alert;
    events.battle_active = on_battle_active;
    events.battle_message = on_battle_message;
    events.allegiances = on_allegiances;
    events.activity = on_activity;
    events.census = on_census;
    events.room_listed = on_room_listed;
    events.room_updated = on_room_updated;
    events.room_entered = on_room_entered;
    events.room_member = on_room_member;
    events.chat_message = on_chat_message;
    events.room_left = on_room_left;
    events.room_created = on_room_created;
    events.file_arrived = on_file_arrived;
    events.battles_done = on_battles_done;
    events.battle_info = on_battle_info;
    events.intel = on_intel;
    events.battle_intel = on_battle_intel;
    events.battle_report = on_battle_report;
    events.launch = on_launch;
    events.debriefing = on_debriefing;
    const BoneyardsService& service = widget->config.service;
    service.set_events(service.user, &events);
}

/// Builds the screen models and listens for the client's events.
///
/// The main menu can wait until the player enters. A second call keeps the
/// models the first call built.
///
/// @param[in,out] widget status widget
void ensure_screens(StatusWidget* widget) {
    if (widget->navigation != nullptr)
        return;
    // Screen views read the default font as they are built.
    toolkit_load_default_font(widget->toolkit);
    widget->navigation = new screens::BoneyardsScreens{};
    screens::boneyards_screens_init(
        widget->navigation, &widget->config.service, widget->config.settings
    );
    widget->navigation->host = screens::ScreensHost{widget, screens_activity_light};
    listen_to_service(widget);
}

/// Opens or closes the on-screen keyboard as the keyboard focus changes.
///
/// A text field passes its place, so the keyboard can stand clear of it.
/// Any other focus, and clearing the focus, closes the keyboard.
void widget_focus_changed(Desktop*, Window* window, void* user) {
    auto* widget = static_cast<StatusWidget*>(user);
    if (widget->screen_keyboard == nullptr)
        return;
    ScopedServices scope(widget);
    TextFieldPlace place{};
    if (window != nullptr && window_text_field_place(window, &place) && place.width > 0 &&
        place.height > 0)
        widget->screen_keyboard(widget->screen_keyboard_user, &place);
    else
        widget->screen_keyboard(widget->screen_keyboard_user, nullptr);
}

/// Loads anims\cursors.gaf's cursornormal and shows it.
///
/// The whole file stays resident after this, so the main menu waits until
/// the pointer is over the strip, a message box is open, or the Boneyards
/// window is up. A second call keeps the cursor already loaded.
///
/// @param[in,out] widget status widget; its services are installed
void load_gui_cursor(StatusWidget* widget) {
    if (widget->system.cursor_ready != 0)
        return;
    const int32_t cursor = cursor_load(k_cursor_file, k_cursor_sequence);
    cursor_set_default(cursor);
    cursor_set(cursor);
    widget->system.cursor_ready = 1;
}

/// The cursor is drawn on this frame, so the file has to be read.
///
/// The idle menu's pointer sits above the strip, and the strip's buffer
/// clips that cursor away. Reading the file then only to discard it is the
/// memory the menu cannot spare.
///
/// @param widget status widget
/// @param placed true when the host reported a pointer position
/// @param pointer_y pointer row in screen pixels
/// @return true when this draw should show the cursor
bool cursor_needed(StatusWidget* widget, bool placed, int32_t pointer_y) {
    if (widget->system.main_frame != nullptr && widget->system.main_frame->base.visible != 0)
        return true;
    if (message_box_find_open(&widget->desktop) != nullptr)
        return true;
    if (!placed)
        return false;
    return pointer_y >= overlay_layout::y && pointer_y < overlay_layout::y + overlay_layout::height;
}

/// Creates the Boneyards GUI manager once.
///
/// Sets up the desktop and image service and keeps them as the console's
/// installed services. A widget that does not defer its models also makes
/// anims\cursors.gaf's cursornormal the default and the shown cursor. The
/// main menu loads that cursor the first time it will be seen. Focusing a
/// text field opens the on-screen keyboard; leaving one closes it.
///
/// @param[in,out] widget status widget
void console_init_gui(StatusWidget* widget) {
    desktop_init(&widget->desktop, 0, 0, k_screen_width, k_screen_height);
    widget->desktop.on_focus = widget_focus_changed;
    widget->desktop.focus_user = widget;
    widget->services.desktop = &widget->desktop;
    widget->services.images = &widget->image_service;
    services_install(widget->services);
    if (!widget->config.defer_models)
        load_gui_cursor(widget);
    widget->system.gui_ready = 1;
}

/// Loads and attaches the online client unless it is already active.
///
/// A failure leaves a message box over the main menu saying which step
/// failed.
///
/// @param[in,out] widget status widget
/// @return true when the client is active
bool console_ensure_client_ready(StatusWidget* widget) {
    ConsoleSystem& system = widget->system;
    if (system.connection_level > 0)
        return true;
    const BoneyardsService& service = widget->config.service;
    const ActivationResult result = service.activate(service.user);
    if (result == ActivationResult::ok) {
        system.connection_level = 1;
        system.pump_enabled = 1;
        return true;
    }
    if (system.main_menu != nullptr && system.gui_ready != 0) {
        const char* text = result == ActivationResult::module_missing ? menu_message::rover_missing
                           : result == ActivationResult::attach_failed
                               ? menu_message::rover_attach_failed
                               : menu_message::rover_activate_failed;
        main_menu_overlay_show_message(system.main_menu, text);
    }
    return false;
}

/// Tells whether the host's message box can hand the focus back: only while the Boneyards GUI is
/// up.
///
/// No Boneyards-side modal dialog exists on the main-menu path.
///
/// @param system the console's system record
/// @return true while the GUI is up
bool console_can_release_dialog(const ConsoleSystem& system) {
    return system.gui_ready != 0;
}

/// Restores the main-menu overlay after a message box while the GUI and the overlay are up.
///
/// @param[in,out] system the console's system record
void console_try_release_dialog(ConsoleSystem* system) {
    if (system->gui_ready != 0 && system->main_menu != nullptr)
        main_menu_overlay_release_dialog(system->main_menu);
}

/// Hides the Boneyards window and drops the graphics only that window draws.
///
/// @param[in,out] widget status widget
void drop_main_frame(StatusWidget* widget) {
    const bool open = widget->system.main_frame != nullptr;
    console_close_main_frame(&widget->system);
    if (open)
        release_window_graphics(widget);
}

// F6 order: the frame, the console that is open, then the war map. A hidden
// console is not a region. Opening a console leaves the keyboard on the
// button that opened it.
constexpr uint8_t k_region_frame = 1;
constexpr uint8_t k_region_console = 2;
constexpr uint8_t k_region_map = 3;

void stamp_keyboard_regions(StatusWidget* widget) {
    MainFrame* frame = widget->system.main_frame;
    if (frame == nullptr) {
        widget->keyboard_console = -1;
        return;
    }
    frame->base.region = k_region_frame;
    for (int32_t i = 0; i < console_screen::none; ++i) {
        Window* kept = frame->kept_screens[i];
        if (kept != nullptr)
            kept->region = 0;
    }
    if (frame->active_screen != nullptr && window_is_shown(frame->active_screen))
        frame->active_screen->region = k_region_console;
    if (frame->map_panel != nullptr)
        frame->map_panel->base.region = k_region_map;

    widget->keyboard_console = frame->console_screen;
}

void destroy_windows(StatusWidget* widget) {
    ConsoleSystem& system = widget->system;
    if (system.main_menu != nullptr)
        console_release_main_menu(&system);
    drop_main_frame(widget);
    services_desktop_destroy_owned();
    while (widget->desktop.windows.count > 0) {
        Window* window = widget->desktop.windows.items[0];
        if (window == nullptr)
            break;
        window_destroy(window);
    }
    close_window_fonts(widget);
}

} // namespace

namespace {

/// Appends one line. A closing line is the bright one.
///
/// @param[in,out] picture the picture being built
/// @param text the line
/// @param closing true for the bright line
void add_line(UpdateCheckPicture& picture, std::string text, bool closing) {
    UpdateCheckLine line;
    line.text = std::move(text);
    line.closing = closing;
    picture.lines.push_back(std::move(line));
}

} // namespace

UpdateCheckPicture boneyards_update_picture(const UpdateCheckFacts& facts) {
    constexpr int32_t full = screens::kUpdatePassFillFull;
    UpdateCheckPicture picture;
    // A ready entry opens the login even when a later check was not run.
    // A stop or a question still holds the screen.
    const auto finish = [&]() -> UpdateCheckPicture {
        if (facts.entry_ready && !picture.stop && !picture.asking)
            picture.finished = true;
        return picture;
    };
    if (!facts.services_checked)
        return finish();
    if (!facts.services_on) {
        add_line(picture, "Online services are off. Turn them on from the Settings page.", true);
        picture.stop = true;
        picture.fill = 0;
        return finish();
    }
    add_line(picture, "Online services are on.", false);
    if (facts.entry_failed && !facts.files_checked) {
        if (!facts.failure.empty())
            add_line(picture, facts.failure, true);
        picture.stop = true;
        return finish();
    }
    if (!facts.files_checked)
        return finish();
    if (!facts.mismatched_file.empty()) {
        add_line(
            picture,
            facts.mismatched_file + " does not match. Restore it from the Settings page.",
            true
        );
        picture.stop = true;
        return finish();
    }
    if (facts.matched_files.empty()) {
        add_line(picture, "Game files match.", false);
    } else {
        for (const std::string& file : facts.matched_files)
            add_line(picture, file + " matches.", false);
    }
    if (!facts.feed_checked) {
        if (facts.entry_failed) {
            if (facts.extension_percent >= 0)
                add_line(picture, "Fetching the Boneyards expansion.", false);
            if (!facts.failure.empty())
                add_line(picture, facts.failure, true);
            picture.stop = true;
        }
        return finish();
    }
    if (facts.feed_failed) {
        add_line(picture, "The update feed did not answer.", false);
    } else if (facts.update_offered && !facts.update_declined) {
        add_line(
            picture,
            "A newer Open Annihilation is available (" + facts.offered_version + ").",
            false
        );
        picture.asking = true;
        return finish();
    } else if (facts.update_offered) {
        add_line(
            picture,
            "A newer Open Annihilation is available (" + facts.offered_version + ").",
            false
        );
    } else {
        add_line(picture, "Open Annihilation is up to date.", false);
    }
    if (facts.entry_failed) {
        if (facts.extension_percent >= 0)
            add_line(picture, "Fetching the Boneyards expansion.", false);
        if (!facts.failure.empty())
            add_line(picture, facts.failure, true);
        picture.stop = true;
        return finish();
    }
    if (!facts.extension_checked)
        return finish();
    if (facts.extension_installed_now) {
        add_line(picture, "Fetching the Boneyards expansion.", false);
        add_line(
            picture, "Installed the Boneyards expansion (" + facts.extension_version + ").", true
        );
        picture.fill = full;
        picture.finished = true;
        return finish();
    }
    if (facts.extension_current) {
        add_line(
            picture,
            "The Boneyards expansion is up to date (" + facts.extension_version + ").",
            true
        );
        picture.fill = full;
        picture.finished = true;
        return finish();
    }
    if (facts.extension_percent >= 0) {
        add_line(picture, "Fetching the Boneyards expansion.", false);
        int32_t percent = facts.extension_percent;
        if (percent < 0)
            percent = 0;
        if (percent > 100)
            percent = 100;
        picture.fill = percent * 10;
        if (picture.fill > full)
            picture.fill = full;
    }
    return finish();
}

void boneyards_status_widget_begin_real_update(StatusWidget* widget) {
    if (widget == nullptr)
        return;
    widget->update_drive = {};
    widget->update_drive.real = true;
}

void boneyards_status_widget_set_real_update(
    StatusWidget* widget, const UpdateCheckPicture& picture
) {
    if (widget == nullptr || !widget->update_drive.real)
        return;
    StatusWidget::UpdateDrive& drive = widget->update_drive;
    drive.lines = picture.lines;
    drive.fill = picture.fill;
    drive.finished = picture.finished;
    drive.stopped = picture.stop;
}

void boneyards_status_widget_set_analogue_atlas_path(const char* path) {
    g_atlas_path = path != nullptr ? path : "";
}

void boneyards_status_widget_set_font_lifetime(
    void (*acquire)(StatusWidget* widget), void (*close_fonts)(StatusWidget* widget)
) {
    g_open_window_fonts = acquire;
    g_close_window_fonts = close_fonts;
}

void boneyards_status_widget_set_text_fonts(StatusWidget* widget, const html::FontService& fonts) {
    if (widget == nullptr)
        return;
    widget->config.text_fonts = fonts;
}

void console_activate_main_menu(StatusWidget* widget, bool connection_pending) {
    ConsoleSystem& system = widget->system;
    if (system.main_menu == nullptr)
        system.main_menu = main_menu_overlay_create(widget);
    if (connection_pending)
        main_menu_overlay_start_activity(system.main_menu);
    system.main_menu->base.visible = 1;
    desktop_set_focus(&widget->desktop, &system.main_menu->base);
}

void console_show_main_menu(StatusWidget* widget, bool connection_pending) {
    if (widget->system.gui_ready == 0)
        console_init_gui(widget);
    console_activate_main_menu(widget, connection_pending);
}

void console_release_main_menu(ConsoleSystem* system) {
    if (system->main_menu == nullptr)
        return;
    system->main_menu->base.visible = 0;
    system->main_menu->base.owned = 1;
    system->main_menu = nullptr;
}

void console_enter_boneyards(StatusWidget* widget) {
    ensure_screens(widget);
    const MainMenuHost& host = widget->config.host;
    if (!console_ensure_client_ready(widget)) {
        call_state(host, frontend_value::state_main_menu);
    } else {
        call_state(host, frontend_value::state_boneyards);
        if (host.update_gadgets != nullptr)
            host.update_gadgets(host.user);
        console_release_main_menu(&widget->system);
        console_open_main_frame(widget);
    }
    if (host.set_signal != nullptr)
        host.set_signal(host.user, 1);
    widget->system.session_started = 1;
}

void console_open_main_frame(StatusWidget* widget) {
    ensure_window_resources(widget);
    ConsoleSystem& system = widget->system;
    if (system.main_frame == nullptr) {
        if (!system.fonts_loaded && widget->toolkit != nullptr) {
            auto& f = system.hat_fonts;
            f.small = font_load("BYHatFontSmall87.gaf");
            f.small_dim = toolkit_font_tint(widget->toolkit, f.small, 0x5a);
            f.small_light = toolkit_font_tint(widget->toolkit, f.small, 0x54);
            tint_role_fonts(widget);
            f.medium = font_load("BYHatFontMedium90.gaf");
            f.button_pressed = toolkit_font_tint(widget->toolkit, f.medium, 0x5d);
            f.button_normal = toolkit_font_tint(widget->toolkit, f.medium, 0x57);
            f.button_hover = toolkit_font_tint(widget->toolkit, f.medium, 0x54);
            f.big = font_load("BYHatFontBig84.gaf");
            system.fonts_loaded = true;
        }
        if (system.info_pane_image == k_no_handle)
            system.info_pane_image = info_pane_image_create();
        if (system.profile_image == k_no_handle)
            system.profile_image = profile_image_create();
        screens::function_button_fonts(&system.hat_fonts);
        system.main_frame = main_frame_create(widget);
        stamp_keyboard_regions(widget);
    }
    if (system.chat_console_pending != 0)
        main_frame_reopen_chat(system.main_frame);

    system.main_frame->base.visible = 1;
    message_box_keep_in_front(&widget->desktop);
}

void console_stop_sounds(const StatusWidget* widget) {
    const MainMenuHost& host = widget->config.host;
    if (host.stop_sounds != nullptr)
        host.stop_sounds(host.user);
}

void console_play_background(const StatusWidget* widget) {
    const MainMenuHost& host = widget->config.host;
    if (widget->navigation != nullptr && widget->navigation->session.background_sound &&
        host.play_sound_alternate != nullptr)
        host.play_sound_alternate(host.user, k_background_sound);
}

void console_close_main_frame(ConsoleSystem* system) {
    if (system->main_frame == nullptr)
        return;
    system->main_frame->base.visible = 0;
    system->main_frame->base.owned = 1;
    system->main_frame = nullptr;
}

namespace {

// Set while a return from a battle is opening the console. That return ends
// the launch itself; a reset reached on the way must not end it as well.
bool g_return_ends_launch = false;

} // namespace

void console_reset_to_main_menu(StatusWidget* widget) {
    ConsoleSystem& system = widget->system;
    const BoneyardsService& service = widget->config.service;
    if (system.connection_level <= 2) {
        if (service.close != nullptr)
            service.close(service.user);
        system.pump_enabled = 0;
        system.connection_level = 0;
    } else if (service.close != nullptr) {
        service.close(service.user);
    }
    drop_main_frame(widget);
    console_show_main_menu(widget, system.connection_level != 0);
    system.matchmaking_enabled = 0;
    const bool launched = system.chat_console_pending != 0;
    system.chat_console_pending = 0;
    const MainMenuHost& host = widget->config.host;
    if (launched && !g_return_ends_launch && host.launch_ended != nullptr)
        host.launch_ended(host.user);
    if (host.init_main_menu_panel != nullptr)
        host.init_main_menu_panel(host.user);
    call_state(host, frontend_value::state_main_menu);
    if (host.set_signal != nullptr)
        host.set_signal(host.user, 1);
    system.session_started = 0;
}

StatusWidget* boneyards_status_widget_create(const StatusWidgetConfig& config) {
    auto* widget = new StatusWidget{};
    widget->config = config;
    boneyards_service_fill_offline(&widget->config.service);
    widget->images = console_images_create(config.files);
    widget->image_service = console_images_service(widget->images);
    attach_toolkit(widget);
    if (!widget->config.defer_models)
        ensure_screens(widget);
    const Services previous = services();
    console_show_main_menu(widget, widget->system.connection_level != 0);
    services_install(previous);
    if (widget->system.main_menu->background < 0) {
        boneyards_status_widget_destroy(widget);
        return nullptr;
    }
    return widget;
}

/// Destroys the overlay, the Main Frame and the GUI manager, and releases the Hat fonts and the
/// pane images.
///
/// @param[in,out] widget status widget
void console_release_gui(StatusWidget* widget) {
    ScopedServices scope(widget);
    destroy_windows(widget);
    desktop_release(&widget->desktop);
    if (widget->system.fonts_loaded) {
        const auto& f = widget->system.hat_fonts;
        const Handle handles[] = {
            f.small_light,
            f.small,
            f.small_dim,
            f.small_green,
            f.small_orange,
            f.small_blue,
            f.small_red,
            f.medium,
            f.button_pressed,
            f.button_normal,
            f.button_hover,
            f.big
        };
        for (Handle font : handles)
            font_release(font);
        release_muzzle_font();
        g_role_font_mode = 0xFF;
        widget->system.hat_fonts = {};
        widget->system.fonts_loaded = false;
        screens::function_button_fonts(nullptr);
    }
    if (widget->system.info_pane_image >= 0)
        image_release(widget->system.info_pane_image);
    widget->system.info_pane_image = k_no_handle;
    if (widget->system.profile_image >= 0)
        image_release(widget->system.profile_image);
    widget->system.profile_image = k_no_handle;
    widget->system.gui_ready = 0;
}

void boneyards_status_widget_destroy(StatusWidget* widget) {
    if (widget == nullptr)
        return;
    console_release_gui(widget);
    const BoneyardsService& service = widget->config.service;
    service.set_events(service.user, nullptr);
    console_images_destroy(widget->images);
    if (widget->toolkit != nullptr) {
        toolkit_set_font_atlas(widget->toolkit, nullptr);
        toolkit_destroy(widget->toolkit);
    }
    font_atlas_close(widget->analogues);
    widget->analogues = nullptr;
    screens::boneyards_use_palette(nullptr, 0);
    if (widget->navigation != nullptr) {
        screens::boneyards_screens_release(widget->navigation);
        delete widget->navigation;
    }
    delete widget;
}

void boneyards_status_widget_leave_main_menu(StatusWidget* widget) {
    if (widget == nullptr)
        return;
    console_release_gui(widget);
    widget->system.session_started = 0;
}

void boneyards_status_widget_return_to_main_menu(StatusWidget* widget) {
    if (widget == nullptr)
        return;
    {
        ScopedServices scope(widget);
        console_show_main_menu(widget, false);
    }
    const MainMenuHost& host = widget->config.host;
    if (host.set_signal != nullptr)
        host.set_signal(host.user, 1);
    widget->system.session_started = 0;
}

bool boneyards_status_widget_event(StatusWidget* widget, const Message& message) {
    if (widget == nullptr || widget->system.gui_ready == 0)
        return false;
    // A console opened over a message box would take the click. The box is
    // put back in front before the click is routed.
    message_box_keep_in_front(&widget->desktop);
    ScopedServices scope(widget);
    const bool pressed = message.code == MessageCode::key_down;
    // A typed character is a single message with no release, so only the
    // control, navigation and modifier keys are tracked as held.
    const bool character = message.key >= k_first_character && message.key <= k_last_character;
    const bool want_shift = widget->event_shift != 0;
    const bool want_control = widget->event_control != 0;
    const bool want_alt = widget->event_alt != 0;
    const bool want_system = widget->event_system != 0;
    widget->event_shift = 0;
    widget->event_control = 0;
    widget->event_alt = 0;
    widget->event_system = 0;
    const bool was_shift = input_key_down(key::shift);
    const bool was_control = input_key_down(key::control);
    const bool was_alt = input_key_down(key::alt);
    const bool was_system = input_key_down(key::system);
    if (widget->toolkit != nullptr) {
        if (want_shift)
            toolkit_set_key(widget->toolkit, key::shift, true);
        if (want_control)
            toolkit_set_key(widget->toolkit, key::control, true);
        if (want_alt)
            toolkit_set_key(widget->toolkit, key::alt, true);
        if (want_system)
            toolkit_set_key(widget->toolkit, key::system, true);
    }
    if (widget->toolkit != nullptr && !character &&
        (pressed || message.code == MessageCode::key_up))
        toolkit_set_key(widget->toolkit, message.key, pressed);
    stamp_keyboard_regions(widget);
    const bool consumed = desktop_route(&widget->desktop, &message);
    if (widget->toolkit != nullptr) {
        if (want_shift && message.key != key::shift)
            toolkit_set_key(widget->toolkit, key::shift, was_shift);
        if (want_control && message.key != key::control)
            toolkit_set_key(widget->toolkit, key::control, was_control);
        if (want_alt && message.key != key::alt)
            toolkit_set_key(widget->toolkit, key::alt, was_alt);
        if (want_system && message.key != key::system)
            toolkit_set_key(widget->toolkit, key::system, was_system);
    }
    return consumed;
}

namespace {

bool send_key(StatusWidget* widget, int32_t key) {
    Message message{};
    message.code = MessageCode::key_down;
    message.key = key;
    const bool consumed = boneyards_status_widget_event(widget, message);
    message.code = MessageCode::key_up;
    boneyards_status_widget_event(widget, message);
    return consumed;
}

} // namespace

bool boneyards_status_widget_pad(StatusWidget* widget, PadPress press) {
    if (widget == nullptr)
        return false;
    switch (press) {
    case PadPress::up:
        return send_key(widget, key::up);
    case PadPress::down:
        return send_key(widget, key::down);
    case PadPress::left:
        return send_key(widget, key::left);
    case PadPress::right:
        return send_key(widget, key::right);
    case PadPress::accept:
    case PadPress::menu:
        return send_key(widget, key::enter);
    case PadPress::back:
        return send_key(widget, key::escape);
    case PadPress::next:
        return send_key(widget, key::tab);
    case PadPress::previous: {
        Message shift{};
        shift.code = MessageCode::key_down;
        shift.key = key::shift;
        boneyards_status_widget_event(widget, shift);
        const bool consumed = send_key(widget, key::tab);
        shift.code = MessageCode::key_up;
        boneyards_status_widget_event(widget, shift);
        return consumed;
    }
    }
    return false;
}

void boneyards_status_widget_modifiers(
    StatusWidget* widget, bool shift, bool control, bool alt, bool system
) {
    if (widget == nullptr || widget->toolkit == nullptr || widget->system.gui_ready == 0)
        return;
    ScopedServices scope(widget);
    toolkit_set_key(widget->toolkit, key::shift, shift);
    toolkit_set_key(widget->toolkit, key::control, control);
    toolkit_set_key(widget->toolkit, key::alt, alt);
    toolkit_set_key(widget->toolkit, key::system, system);
}

void boneyards_status_widget_event_modifiers(
    StatusWidget* widget, bool shift, bool control, bool alt, bool system
) {
    if (widget == nullptr)
        return;
    widget->event_shift = shift ? 1 : 0;
    widget->event_control = control ? 1 : 0;
    widget->event_alt = alt ? 1 : 0;
    widget->event_system = system ? 1 : 0;
}

void boneyards_status_widget_set_screen_keyboard(
    StatusWidget* widget, void (*callback)(void* user, const TextFieldPlace* place), void* user
) {
    if (widget == nullptr)
        return;
    widget->screen_keyboard = callback;
    widget->screen_keyboard_user = user;
}

namespace {

bool is_text_field(const Window* window) {
    return window != nullptr && window->ops != nullptr && window->ops->is_class != nullptr &&
           (window->ops->is_class(class_name::single_edit) ||
            window->ops->is_class(class_name::edit_box));
}

Window* focused_field(StatusWidget* widget) {
    if (widget == nullptr)
        return nullptr;
    Window* focus = widget->desktop.focus;
    return is_text_field(focus) ? focus : nullptr;
}

} // namespace

bool boneyards_status_widget_take_text(StatusWidget* widget, const char* text) {
    Window* focus = focused_field(widget);
    if (focus == nullptr)
        return false;
    ScopedServices scope(widget);
    if (focus->ops->is_class(class_name::single_edit))
        single_edit_insert_text(reinterpret_cast<SingleEdit*>(focus), text);
    else
        edit_box_insert_text(reinterpret_cast<EditBox*>(focus), text);
    return true;
}

void boneyards_status_widget_set_composition(StatusWidget* widget, const char* text) {
    Window* focus = focused_field(widget);
    if (focus == nullptr)
        return;
    if (focus->ops->is_class(class_name::single_edit))
        single_edit_set_composition(reinterpret_cast<SingleEdit*>(focus), text);
    else
        edit_box_set_composition(reinterpret_cast<EditBox*>(focus), text);
}

bool boneyards_status_widget_text_field(
    const StatusWidget* widget,
    int32_t* x,
    int32_t* y,
    int32_t* width,
    int32_t* height,
    int32_t* cursor
) {
    if (widget == nullptr)
        return false;
    // Measuring the caret uses the widget's text service.
    StatusWidget* editable = const_cast<StatusWidget*>(widget);
    Window* focus = focused_field(editable);
    if (focus == nullptr)
        return false;
    ScopedServices scope(editable);
    TextFieldPlace place{};
    if (!window_text_field_place(focus, &place))
        return false;
    if (x != nullptr)
        *x = place.x;
    if (y != nullptr)
        *y = place.y;
    if (width != nullptr)
        *width = place.width;
    if (height != nullptr)
        *height = place.height;
    if (cursor != nullptr)
        *cursor = place.cursor;
    return true;
}

void boneyards_status_widget_tick(StatusWidget* widget) {
    if (widget == nullptr)
        return;
    ScopedServices scope(widget);
    const MainMenuHost& host = widget->config.host;
    bool elapsed = true;
    if (host.current_tick != nullptr) {
        const uint32_t now = host.current_tick(host.user);
        oa::boneyards::rover_session_note_clock(now);
        elapsed = now != widget->last_tick;
        widget->last_tick = now;
    }
    const BoneyardsService& service = widget->config.service;
    service.pump(service.user);
    if (elapsed) {
        // The windows' cycle stands still while a launched battle is under way.
        if (widget->system.chat_console_pending == 0) {
            stamp_keyboard_regions(widget);
            desktop_pulse(&widget->desktop);
        }
        if (widget->toolkit != nullptr)
            toolkit_step_cursor(widget->toolkit);
    }
    desktop_destroy_owned(&widget->desktop);
    close_window_fonts(widget);
    message_box_keep_in_front(&widget->desktop);
}

namespace {

void offset_window(Window* window, int32_t dy) {
    if (window == nullptr)
        return;
    window->y += dy;
    for (int32_t i = 0; i < window->children.count; ++i)
        offset_window(window->children.items[i], dy);
}

void offset_desktop(Desktop* desktop, int32_t dy) {
    for (int32_t i = 0; i < desktop->windows.count; ++i)
        offset_window(desktop->windows.items[i], dy);
}

/// Subtracts `origin_y` from every window for the draw, then puts it back.
struct DesktopShift {
    Desktop* desktop;
    int32_t origin_y;

    DesktopShift(Desktop* desktop_in, int32_t origin_in)
        : desktop(desktop_in), origin_y(origin_in) {
        if (origin_y != 0)
            offset_desktop(desktop, -origin_y);
    }

    ~DesktopShift() {
        if (origin_y != 0)
            offset_desktop(desktop, origin_y);
    }

    DesktopShift(const DesktopShift&) = delete;
    DesktopShift& operator=(const DesktopShift&) = delete;
};

} // namespace

void boneyards_status_widget_draw(StatusWidget* widget, Surface* target, int32_t origin_y) {
    if (widget == nullptr || !desktop_has_windows(&widget->desktop))
        return;
    if (widget->config.palette != nullptr)
        screens::boneyards_use_palette(
            reinterpret_cast<const uint8_t*>(widget->config.palette->entries), sizeof(oa::Palette)
        );
    if (widget->galaxy_graphics != nullptr)
        widget->galaxy_graphics->palette = widget->config.palette;
    refresh_role_fonts(widget);
    message_box_keep_in_front(&widget->desktop);
    ScopedServices scope(widget);
    const DesktopShift shift(&widget->desktop, origin_y);
    console_images_set_target(widget->images, target);
    if (widget->toolkit != nullptr) {
        toolkit_set_target(widget->toolkit, target);
        const MainMenuHost& host = widget->config.host;
        int32_t x = 0;
        int32_t y = 0;
        const bool placed =
            host.pointer_position != nullptr && host.pointer_position(host.user, &x, &y);
        // Both passes of one frame load before they draw, so the cursor is
        // in each pass and the two passes still agree.
        if (widget->system.cursor_ready == 0 && cursor_needed(widget, placed, y))
            load_gui_cursor(widget);
        if (placed)
            toolkit_draw_cursor(widget->toolkit, x, y - origin_y);
    }
    desktop_draw(&widget->desktop);
    if (widget->toolkit != nullptr)
        toolkit_set_target(widget->toolkit, nullptr);
    console_images_set_target(widget->images, nullptr);
}

void boneyards_status_widget_show_message(StatusWidget* widget, const char* text) {
    if (widget == nullptr || widget->system.main_menu == nullptr)
        return;
    ScopedServices scope(widget);
    main_menu_overlay_show_message(widget->system.main_menu, text);
}

bool boneyards_status_widget_message_open(StatusWidget* widget) {
    return widget != nullptr && message_box_find_open(&widget->desktop) != nullptr;
}

bool boneyards_status_widget_message_button(
    StatusWidget* widget, const char* name, int32_t* x, int32_t* y
) {
    MessageBox* box = widget != nullptr ? message_box_find_open(&widget->desktop) : nullptr;
    const Window* button = box != nullptr ? window_find_named(&box->base, name, nullptr) : nullptr;
    if (button == nullptr)
        return false;
    *x = button->x + button->width / 2;
    *y = button->y + button->height / 2;
    return true;
}

void boneyards_status_widget_dialog_closed(StatusWidget* widget) {
    if (widget == nullptr)
        return;
    ScopedServices scope(widget);
    if (console_can_release_dialog(widget->system))
        console_try_release_dialog(&widget->system);
}

bool boneyards_status_widget_visible(const StatusWidget* widget) {
    return widget != nullptr && widget->system.main_menu != nullptr &&
           widget->system.main_menu->base.visible != 0;
}

bool boneyards_status_widget_in_boneyards(const StatusWidget* widget) {
    return widget != nullptr && widget->system.main_frame != nullptr &&
           widget->system.main_frame->base.visible != 0;
}

bool boneyards_status_widget_launched_game(const StatusWidget* widget) {
    return widget != nullptr && widget->system.chat_console_pending != 0;
}

bool boneyards_status_widget_battle_ids(
    const StatusWidget* widget, uint32_t* area_id, const char** game_id
) {
    if (widget == nullptr)
        return false;
    *area_id = widget->system.launched_area_id;
    *game_id = widget->system.launched_game_id;
    return true;
}

bool boneyards_status_widget_battle_place(
    const StatusWidget* widget, const char** planet, const char** sector
) {
    if (widget == nullptr)
        return false;
    *planet = widget->system.launched_planet;
    *sector = widget->system.launched_sector;
    return true;
}

bool boneyards_status_widget_network_available(const StatusWidget* widget) {
    if (widget == nullptr)
        return false;
    const BoneyardsService& service = widget->config.service;
    return service.network_available != nullptr && service.network_available(service.user);
}

void boneyards_status_widget_enter(StatusWidget* widget) {
    if (widget == nullptr)
        return;
    ScopedServices scope(widget);
    if (widget->system.gui_ready == 0)
        console_init_gui(widget);
    console_enter_boneyards(widget);
}

void boneyards_status_widget_return_from_battle(StatusWidget* widget) {
    if (widget == nullptr)
        return;
    ensure_screens(widget);
    ScopedServices scope(widget);
    ConsoleSystem& system = widget->system;
    const MainMenuHost& host = widget->config.host;
    // One leave for this battle: the battle room's back button and a failed
    // join ("Host not found") both come back through here. A second return
    // finds the game id already cleared and does not leave again.
    if (host.left_game != nullptr && system.launched_game_id[0] != '\0') {
        host.left_game(host.user, system.launched_area_id, system.launched_game_id);
        system.launched_game_id[0] = '\0';
    }
    if (system.gui_ready == 0)
        console_init_gui(widget);
    console_release_main_menu(&system);
    if (host.update_gadgets != nullptr)
        host.update_gadgets(host.user);
    // Remember the launch before the console is rebuilt. Opening the frame
    // can clear the flag on its way back to the main menu; this return is
    // what ends the launch.
    const bool launched = system.chat_console_pending != 0;
    g_return_ends_launch = launched;
    console_open_main_frame(widget);
    g_return_ends_launch = false;
    system.chat_console_pending = 0;
    if (launched && host.launch_ended != nullptr)
        host.launch_ended(host.user);
    widget->navigation->session.debriefing_pending = false;
    console_stop_sounds(widget);
    call_state(host, frontend_value::state_boneyards);
    if (host.set_signal != nullptr)
        host.set_signal(host.user, 1);
    system.session_started = 1;
    if (!widget->held_notice.empty()) {
        const std::string notice = std::move(widget->held_notice);
        widget->held_notice.clear();
        message_box_create(notice.c_str(), message_box_buttons::ok, nullptr, nullptr, nullptr);
    }
}

void boneyards_status_widget_show_notice(StatusWidget* widget, const char* text) {
    if (widget == nullptr)
        return;
    if (widget->system.gui_ready == 0) {
        widget->held_notice = text != nullptr ? text : "";
        return;
    }
    ScopedServices scope(widget);
    message_box_create(text, message_box_buttons::ok, nullptr, nullptr, nullptr);
}

void boneyards_status_widget_visit_driver_controls(
    const StatusWidget* widget, void* context, DriverControlVisitor visit
) {
    if (widget == nullptr || visit == nullptr)
        return;
    message_box_keep_in_front(&const_cast<StatusWidget*>(widget)->desktop);
    ScopedServices scope(const_cast<StatusWidget*>(widget));
    desktop_visit_driver_controls(&widget->desktop, context, visit);
}

void boneyards_status_widget_idle(StatusWidget* widget, uint32_t now_ms, bool pump) {
    if (widget == nullptr)
        return;
    ScopedServices scope(widget);
    if (pump) {
        const BoneyardsService& service = widget->config.service;
        service.pump(service.user);
    }
    ConsoleSystem& system = widget->system;
    if (system.gui_ready == 0)
        return;
    const MainMenuHost& host = widget->config.host;
    if (system.exit_declined) {
        system.exit_declined = false;
    } else if (system.exit_accepted) {
        system.exit_accepted = false;
        if (host.leave_game != nullptr)
            host.leave_game(host.user);
    }
    if (system.chat_console_pending == 0)
        return;
    if (system.just_launched != 0) {
        if (host.empty_chat_ring != nullptr)
            host.empty_chat_ring(host.user);
        if (host.set_latency_reply_flags != nullptr)
            host.set_latency_reply_flags(host.user, 0);
    }
    if (now_ms - system.last_check_ms > k_check_period_ms) {
        system.last_check_ms = now_ms;
        system.just_launched = 0;
    }
}

bool boneyards_status_widget_up(const StatusWidget* widget) {
    return widget != nullptr && widget->system.gui_ready != 0 &&
           widget->system.session_started != 0;
}

bool boneyards_status_widget_ask_exit(StatusWidget* widget) {
    if (widget == nullptr || widget->system.gui_ready == 0)
        return false;
    ScopedServices scope(widget);
    ConsoleSystem& system = widget->system;
    message_box_create(
        menu_message::exit_to_windows,
        message_box_buttons::yes_no,
        &system.exit_accepted,
        &system.exit_declined,
        nullptr
    );
    return true;
}

void boneyards_status_widget_stop_sounds(const StatusWidget* widget) {
    if (widget != nullptr)
        console_stop_sounds(widget);
}

Window* boneyards_status_widget_part(StatusWidget* widget, OverlayPart part) {
    if (widget == nullptr || widget->system.main_menu == nullptr)
        return nullptr;
    MainMenuOverlay* overlay = widget->system.main_menu;
    switch (part) {
    case OverlayPart::panel:
        return &overlay->base;
    case OverlayPart::single:
        return overlay->single_button;
    case OverlayPart::multi:
        return overlay->multi_button;
    case OverlayPart::intro:
        return overlay->intro_button;
    case OverlayPart::exit:
        return overlay->exit_button;
    case OverlayPart::credits:
        return overlay->credits_button;
    case OverlayPart::enter:
        return overlay->enter_button;
    case OverlayPart::activity_light:
        return &overlay->activity_light->base;
    }
    return nullptr;
}

ConsoleImages* boneyards_status_widget_images(StatusWidget* widget) {
    return widget != nullptr ? widget->images : nullptr;
}

bool boneyards_status_widget_probe(const StatusWidget* widget, MainFrameProbe* out) {
    const MainFrame* frame = widget != nullptr ? widget->system.main_frame : nullptr;
    if (frame == nullptr || widget->navigation == nullptr)
        return false;
    const ConsoleLayer& layer = *frame->console;
    const screens::GalaxyView& galaxy = widget->navigation->galaxy;
    const auto state = [frame](ConsoleButton which) {
        return frame->console_buttons[static_cast<int32_t>(which)]->states.current;
    };
    *out = MainFrameProbe{};
    out->connection_level = widget->system.connection_level;
    oa::boneyards::utf8::copy(
        out->user_name,
        sizeof out->user_name,
        widget->system.user_name,
        oa::boneyards::kPlayerNameCharacters
    );
    out->console_screen = frame->console_screen;
    out->screen_shown = frame->active_screen != nullptr && window_is_shown(frame->active_screen);
    out->layer_sliding = layer.sliding != 0;
    out->layer_direction = static_cast<int32_t>(layer.direction);
    out->layer_covering = layer.covering != 0;
    out->layer_countdown = layer.countdown;
    out->slide_steps = layer.slide_steps;
    out->planet_count = galaxy.planet_count;
    out->planets_drawn = galaxy_panel_planets_drawn(frame);
    out->galaxy_loading = galaxy.loading;
    out->map_locked = galaxy.locked;
    out->selected_planet = galaxy.selected;
    out->unlock_pending = frame->unlock_pending != 0;
    out->release_button = state(ConsoleButton::release);
    out->recon_button = state(ConsoleButton::recon);
    out->chat_button = state(ConsoleButton::chat);
    out->battle_button = state(ConsoleButton::battle);
    out->activity_light_running = frame->activity_light->running != 0;
    std::snprintf(out->ticker_text, sizeof out->ticker_text, "%s", frame->ticker);
    read_label(widget, frame->ticker_label, out->ticker_shown, sizeof out->ticker_shown);
    const BattleSummary* summary = frame->battle_summary;
    out->summary_shown = summary != nullptr && window_is_shown(summary->name);
    out->summary_planet = galaxy.territory.planet;
    if (summary != nullptr && summary->background.pixels != nullptr) {
        const Surface& b = summary->background;
        for (int32_t y = k_summary_picture_top; y <= k_summary_picture_bottom && y < b.height; ++y)
            for (int32_t x = k_summary_picture_left; x <= k_summary_picture_right && x < b.width;
                 ++x)
                out->summary_picture_pixels +=
                    b.pixels[y * b.pitch + x] != k_summary_picture_fill ? 1U : 0U;
    }
    const auto read = [widget](const Window* label, char* text, std::size_t capacity) {
        read_label(widget, label, text, capacity);
    };
    if (summary != nullptr) {
        read(summary->name, out->summary_name, sizeof out->summary_name);
        read(summary->owner, out->summary_owner, sizeof out->summary_owner);
        read(summary->battles, out->summary_battles, sizeof out->summary_battles);
        read(summary->forming, out->summary_forming, sizeof out->summary_forming);
        out->summary_wins_shown = window_is_shown(summary->arm_wins);
        read(summary->arm_wins, out->summary_arm_wins, sizeof out->summary_arm_wins);
        read(summary->core_wins, out->summary_core_wins, sizeof out->summary_core_wins);
    }
    if (galaxy.selected >= 0 && galaxy.selected < static_cast<int32_t>(galaxy.planet_count)) {
        const screens::GalaxyPlanet& selected = galaxy.planets[galaxy.selected];
        // The planet's name need not end within its field.
        const std::size_t name_length =
            std::min(::strnlen(selected.name, sizeof selected.name), sizeof out->selected_name - 1);
        std::memcpy(out->selected_name, selected.name, name_length);
        out->selected_name[name_length] = '\0';
        out->selected_side = selected.side;
        out->selected_census = CensusReport{
            static_cast<uint32_t>(selected.id),
            selected.population,
            selected.activity,
            selected.in_progress,
            selected.forming,
            selected.core_wins,
            selected.arm_wins
        };
    }
    for (int32_t i = 0; i < k_console_button_count; ++i) {
        const Window* button = frame->console_buttons[i];
        out->console_buttons[i] = ScreenRect{button->x, button->y, button->width, button->height};
    }
    out->selected_area_id = widget->navigation->session.area_id;
    out->census_requests = frame->census_requests;
    if (frame->console_screen == console_screen::recon && frame->active_screen != nullptr &&
        widget->config.screens.create == nullptr) {
        const auto* recon = reinterpret_cast<const ReconConsole*>(frame->active_screen);
        namespace row = screens::planet_info_row;
        out->recon_shown = window_is_shown(&recon->base);
        read(
            recon->fields[row::planet]->value, out->recon_planet_text, sizeof out->recon_planet_text
        );
        read(
            recon->fields[row::gravity]->value,
            out->recon_gravity_text,
            sizeof out->recon_gravity_text
        );
        char population[12]{};
        read(recon->fields[row::population]->value, population, sizeof population);
        out->recon_population = static_cast<int32_t>(std::strtol(population, nullptr, 10));
        out->recon_panorama = recon->state.map_installed && recon->state.has_pan;
        out->recon_pan_scroll = recon->state.pan_scroll;
    }
    if (frame->console_screen == console_screen::profile) {
        const screens::ProfileConsole& profile = widget->navigation->profile;
        const char* description = profile.tab_description;
        std::snprintf(
            out->profile_tab_description,
            sizeof out->profile_tab_description,
            "%s",
            description != nullptr ? description : ""
        );
        out->profile_receiving = profile.busy;
        if (profile.record != nullptr)
            oa::boneyards::utf8::copy(
                out->profile_login_name, sizeof out->profile_login_name, profile.record->name, 0
            );
        if (frame->active_screen != nullptr && widget->config.screens.create == nullptr) {
            const auto* view = reinterpret_cast<const ProfileView*>(frame->active_screen);
            const Window* row = view->list->selected_item;
            out->profile_shown = window_is_shown(&view->base);
            out->profile_rows = view->list->list.count;
            out->profile_row_tab =
                row != nullptr ? reinterpret_cast<const screens::ByCircleRow*>(row)->tab : -1;
            read(
                view->description[0],
                out->profile_description_line,
                sizeof out->profile_description_line
            );
            out->profile_status_shown = window_is_shown(view->status);
            read(view->status, out->profile_status, sizeof out->profile_status);
        }
    }
    const int32_t shown = frame->console_screen;
    if (frame->active_screen != nullptr && widget->config.screens.create == nullptr &&
        (shown == console_screen::help || shown == console_screen::ladders ||
         shown == console_screen::news))
        contents_console_probe(frame->active_screen, out);
    if (const ChatView* view = console_open_chat(); view != nullptr) {
        ListBox* log = &view->log->list;
        out->chat_lines = log->count;
        const auto* last =
            log->count > 0
                ? reinterpret_cast<const screens::ByTextRow*>(list_box_item(log, log->count - 1))
                : nullptr;
        if (last != nullptr)
            read(last->label, out->chat_last_line, sizeof out->chat_last_line);
        out->chat_frequency_rows = view->room_list->list.count;
        out->chat_player_rows = view->player_list->list.count;
        const Window* create = view->create_button;
        out->chat_create_button = ScreenRect{create->x, create->y, create->width, create->height};
    }
    const screens::ChatConsole& chat = widget->navigation->chat;
    out->chat_room_id = chat.session->room_id;
    out->chat_members = static_cast<int32_t>(chat.player_total);
    out->chat_frequencies = static_cast<int32_t>(chat.room_total);
    out->chat_pending_room_id = chat.session->pending_room_id;
    out->chat_pane_user_id = chat.pane.user_id;
    copy_text(out->chat_pane_name, sizeof out->chat_pane_name, chat.pane.name);
    copy_text(out->chat_pane_clan, sizeof out->chat_pane_clan, chat.pane.clan);
    return true;
}

bool boneyards_status_widget_chat_row(
    const StatusWidget* widget, ChatList list, int32_t index, ChatRowProbe* out
) {
    const ChatView* view = widget != nullptr ? console_open_chat() : nullptr;
    if (view == nullptr)
        return false;
    screens::ByListBox* rows = list == ChatList::frequencies ? view->room_list : view->player_list;
    Window* row = list_box_item(&rows->list, index);
    if (row == nullptr)
        return false;
    *out = ChatRowProbe{};
    out->shown = window_is_shown(row);
    out->x = row->x + row->width / 2;
    out->y = row->y + row->height / 2;
    out->selected = rows->selected_item == row;
    if (row->ops->is_class(screens::kByChatRoomRowClass)) {
        const auto* room = reinterpret_cast<const screens::ByChatRoomRow*>(row);
        out->id = room->room_id;
        out->type = room->type;
        out->current = room->current_room != nullptr && *room->current_room == room->room_id;
        read_label(widget, room->name_label, out->name, sizeof out->name);
        read_label(widget, room->count_label, out->count, sizeof out->count);
    } else if (row->ops->is_class(screens::kByPlayerRowClass)) {
        const auto* player = reinterpret_cast<const screens::ByPlayerRow*>(row);
        out->id = player->user_id;
        read_label(widget, player->name_label, out->name, sizeof out->name);
    }
    return true;
}

bool boneyards_status_widget_chat_pane(
    const StatusWidget* widget, ChatPane pane, ChatPaneProbe* out
) {
    const ChatView* view = widget != nullptr ? console_open_chat() : nullptr;
    if (view == nullptr)
        return false;
    const auto rect = [](const Window* w) { return ScreenRect{w->x, w->y, w->width, w->height}; };
    const auto entry = [&](int32_t i, const SingleEdit* edit) {
        out->entries[i] = rect(&edit->base);
        out->entry_shown[i] = window_is_shown(&edit->base);
    };
    *out = ChatPaneProbe{};
    if (pane == ChatPane::player) {
        const PlayerPaneView* player = view->player_pane;
        if (player == nullptr)
            return false;
        out->shown = window_is_shown(&player->pane.base);
        out->go = rect(player->page);
        out->go_state = player->page->states.current;
        read_label(widget, player->name, out->name, sizeof out->name);
        read_label(widget, player->rank, out->rank, sizeof out->rank);
        read_label(widget, player->clan, out->clan, sizeof out->clan);
        return true;
    }
    if (pane == ChatPane::create_frequency) {
        const CreateFrequencyPane* create = view->create_pane;
        if (create == nullptr)
            return false;
        out->shown = window_is_shown(&create->pane.base);
        entry(0, create->name);
        entry(1, create->password);
        entry(2, create->description);
        out->go = rect(create->go);
        out->go_state = create->go->states.current;
        return true;
    }
    const FrequencyInfoPane* info = view->info_pane;
    if (info == nullptr)
        return false;
    out->shown = window_is_shown(&info->pane.base);
    entry(0, info->password);
    out->go = rect(info->go);
    out->go_state = info->go->states.current;
    read_label(widget, info->name_label, out->name, sizeof out->name);
    read_label(widget, info->type->value, out->type, sizeof out->type);
    read_label(widget, info->creator->value, out->creator, sizeof out->creator);
    read_label(widget, info->population_field->value, out->population, sizeof out->population);
    return true;
}

bool boneyards_status_widget_chat_log_line(
    const StatusWidget* widget, int32_t index, char* text, std::size_t capacity
) {
    const ChatView* view = widget != nullptr ? console_open_chat() : nullptr;
    const auto* row =
        view != nullptr
            ? reinterpret_cast<const screens::ByTextRow*>(list_box_item(&view->log->list, index))
            : nullptr;
    if (row == nullptr)
        return false;
    read_label(widget, row->label, text, capacity);
    return true;
}

namespace {

// The battle console while it is the shown screen; null otherwise.
BattleConsole* open_battle_console(const StatusWidget* widget) {
    const MainFrame* frame = widget != nullptr ? widget->system.main_frame : nullptr;
    if (frame == nullptr || frame->console_screen != console_screen::battle ||
        frame->active_screen == nullptr || !window_is_shown(frame->active_screen))
        return nullptr;
    return reinterpret_cast<BattleConsole*>(frame->active_screen);
}

ScreenRect window_rect(const Window* window) {
    if (window == nullptr)
        return ScreenRect{};
    return ScreenRect{window->x, window->y, window->width, window->height};
}

} // namespace

bool boneyards_status_widget_battle_row(
    const StatusWidget* widget, int32_t index, BattleRowProbe* out
) {
    BattleConsole* console = open_battle_console(widget);
    if (console == nullptr || out == nullptr || widget->navigation == nullptr)
        return false;
    const screens::BattleList* battles = &widget->navigation->battles;
    if (battles->rows_dirty || console->list == nullptr)
        return false;
    Window* row = list_box_item(&console->list->list, index);
    if (row == nullptr || index < 0 || static_cast<std::size_t>(index) >= battles->row_count)
        return false;
    const screens::BattleRow& model = battles->rows[index];
    *out = BattleRowProbe{};
    out->shown = window_is_shown(row);
    out->x = row->x + row->width / 2;
    out->y = row->y + row->height / 2;
    out->category = model.is_category;
    out->selected = console->list->selected_item == row;
    const auto* text = reinterpret_cast<const screens::ByTextRow*>(row);
    read_label(widget, text->label, out->name, sizeof out->name);
    if (!model.is_category) {
        const screens::BattleEntry* entry = screens::battle_list_entry(battles, model.battle);
        if (entry != nullptr)
            copy_text(out->game_id, sizeof out->game_id, entry->game_id);
    }
    return true;
}

bool boneyards_status_widget_battle_type(
    const StatusWidget* widget, int32_t index, BattleTypeProbe* out
) {
    BattleConsole* console = open_battle_console(widget);
    if (console == nullptr || out == nullptr)
        return false;
    auto* host = reinterpret_cast<HostBattlePane*>(
        console->panes[static_cast<int32_t>(screens::BattlePanel::host)]
    );
    if (host == nullptr || host->types == nullptr || !window_is_shown(&host->pane.base))
        return false;
    Window* row = list_box_item(&host->types->list, index);
    if (row == nullptr)
        return false;
    *out = BattleTypeProbe{};
    out->shown = window_is_shown(row);
    out->x = row->x + row->width / 2;
    out->y = row->y + row->height / 2;
    out->selected = host->types->selected_item == row;
    const auto* text = reinterpret_cast<const screens::ByTextRow*>(row);
    read_label(widget, text->label, out->name, sizeof out->name);
    return true;
}

bool boneyards_status_widget_battle_controls(const StatusWidget* widget, BattleControlsProbe* out) {
    BattleConsole* console = open_battle_console(widget);
    if (console == nullptr || out == nullptr)
        return false;
    *out = BattleControlsProbe{};
    if (console->host_button != nullptr) {
        out->host_button_shown = window_is_shown(console->host_button);
        out->host_button = window_rect(console->host_button);
    }
    auto* host = reinterpret_cast<HostBattlePane*>(
        console->panes[static_cast<int32_t>(screens::BattlePanel::host)]
    );
    if (host != nullptr && host->game_name != nullptr && host->start != nullptr) {
        out->host_pane_shown = window_is_shown(&host->pane.base);
        out->name_entry = window_rect(&host->game_name->base);
        copy_text(out->name_text, sizeof out->name_text, single_edit_text(host->game_name));
        out->start_button = window_rect(host->start);
        out->start_state = host->start->states.current;
    }
    auto* join = reinterpret_cast<JoinSummaryPane*>(
        console->panes[static_cast<int32_t>(screens::BattlePanel::join)]
    );
    if (join != nullptr && join->join != nullptr) {
        out->join_pane_shown = window_is_shown(&join->pane.base);
        out->join_button = window_rect(join->join);
        out->join_state = join->join->states.current;
    }
    return true;
}

bool boneyards_status_widget_planet_point(
    const StatusWidget* widget, std::size_t planet, int32_t* x, int32_t* y
) {
    const MainFrame* frame = widget != nullptr ? widget->system.main_frame : nullptr;
    if (frame == nullptr || frame->map_panel == nullptr || planet >= screens::kMaxGalaxyPlanets)
        return false;
    const Window* hotspot = frame->map_panel->hotspots[planet];
    if (hotspot == nullptr || !window_is_shown(hotspot))
        return false;
    *x = hotspot->x + hotspot->width / 2;
    *y = hotspot->y + hotspot->height / 2;
    return true;
}

bool boneyards_status_widget_planet_hotspot(
    const StatusWidget* widget, int32_t area_id, int32_t* x, int32_t* y
) {
    if (widget == nullptr || widget->navigation == nullptr || widget->system.main_frame == nullptr)
        return false;
    const int32_t planet = screens::galaxy_find_planet(&widget->navigation->galaxy, area_id);
    return planet != screens::kNoPlanet &&
           boneyards_status_widget_planet_point(widget, static_cast<std::size_t>(planet), x, y);
}

namespace {

void (*g_driver_note)(const char*, const char*) = nullptr;

void write_planet_text(char* out, std::size_t capacity, const screens::GalaxyPlanet& planet) {
    const char owner = planet.side != 0 ? planet.side : '?';
    std::snprintf(
        out,
        capacity,
        "id=%d owner=%c battles=%d forming=%d",
        planet.id,
        owner,
        planet.in_progress,
        planet.forming
    );
}

void note_opened_box(const char* text) {
    boneyards_console_driver_note("alert", text);
}

} // namespace

void boneyards_console_set_driver_note(void (*note)(const char*, const char*)) {
    g_driver_note = note;
    message_box_set_opened(note != nullptr ? note_opened_box : nullptr);
}

void boneyards_console_driver_note(const char* kind, const char* text) {
    if (g_driver_note == nullptr || kind == nullptr || kind[0] == '\0')
        return;
    g_driver_note(kind, text != nullptr ? text : "");
}

std::size_t boneyards_status_widget_planet_facts(
    const StatusWidget* widget, DriverPlanetFact* out, std::size_t capacity
) {
    if (widget == nullptr || out == nullptr || capacity == 0 || widget->navigation == nullptr ||
        widget->system.main_frame == nullptr)
        return 0;
    const screens::GalaxyView& galaxy = widget->navigation->galaxy;
    const GalaxyPanel* panel = widget->system.main_frame->map_panel;
    std::size_t written = 0;
    const std::size_t count = galaxy.planet_count;
    for (std::size_t index = 0; index < count && written < capacity; ++index) {
        const screens::GalaxyPlanet& planet = galaxy.planets[index];
        DriverPlanetFact& fact = out[written];
        fact = DriverPlanetFact{};
        fact.id = planet.id;
        galaxy_panel_planet_label(galaxy, index, fact.label, sizeof fact.label);
        const Window* hotspot = panel != nullptr && index < screens::kMaxGalaxyPlanets
                                    ? panel->hotspots[index]
                                    : nullptr;
        if (hotspot != nullptr && window_is_shown(hotspot)) {
            fact.on_viewer = true;
            fact.x = hotspot->x;
            fact.y = hotspot->y;
            fact.width = hotspot->width;
            fact.height = hotspot->height;
            const auto* button = reinterpret_cast<const PlanetHotspot*>(hotspot);
            std::snprintf(fact.text, sizeof fact.text, "%s", button->facts);
        } else {
            write_planet_text(fact.text, sizeof fact.text, planet);
        }
        ++written;
    }
    return written;
}

bool boneyards_status_widget_map_caption(const StatusWidget* widget, DriverMapCaption* out) {
    if (out == nullptr)
        return false;
    *out = DriverMapCaption{};
    if (widget == nullptr || widget->navigation == nullptr || widget->system.main_frame == nullptr)
        return false;
    const screens::GalaxyView& galaxy = widget->navigation->galaxy;
    std::snprintf(out->sector, sizeof out->sector, "%s", galaxy.sector_name);
    out->magnification = galaxy.scale;
    out->ready = true;
    return true;
}

bool boneyards_status_widget_banner(const StatusWidget* widget, DriverBannerFact* out) {
    if (out == nullptr)
        return false;
    *out = DriverBannerFact{};
    const MainFrame* frame = widget != nullptr ? widget->system.main_frame : nullptr;
    if (frame == nullptr || frame->ad_banner == nullptr || !window_is_shown(frame->ad_banner))
        return false;
    out->shown = true;
    out->x = frame->ad_banner->x;
    out->y = frame->ad_banner->y;
    out->width = frame->ad_banner->width;
    out->height = frame->ad_banner->height;
    ad_banner_shown_text(
        reinterpret_cast<const AdBanner*>(frame->ad_banner), out->identity, sizeof out->identity
    );
    return true;
}

} // namespace oa::boneyards::console
