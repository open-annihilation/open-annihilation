// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Central Command session: login, dossier, disconnect and logout as
// the game drives them through the Rover calls, reported to the screens
// through ServiceEvents.
#include "oa/boneyards/rover_session.hpp"

#include "oa/boneyards/overlay_launch.hpp"
#include "oa/boneyards/shown_text.hpp"
#include "oa/boneyards/network_link.hpp"
#include "oa/boneyards/profile_templates.hpp"
#include "oa/boneyards/utf8.hpp"
#include "oa/boneyards/release.hpp"
#include "oa/boneyards/rover_calls.hpp"
#include "oa/boneyards/rover_string.hpp"
#include "oa/rover/launch_overlay.h"
#include "handlers.h"
#include "record.h"
#include "lobby.h"
#include "sector_fetch.h"
#include "servers.h"
#include "oa/boneyards/screens/profile.hpp"
#include "oa/platform/files.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace oa::boneyards {
namespace {

// The war-map records are Rover's, passed through unchanged.
static_assert(sizeof(SectorPlanetRecord) == sizeof(RoverSectorPlanet));
static_assert(offsetof(SectorPlanetRecord, seqname) == offsetof(RoverSectorPlanet, seqname));
static_assert(offsetof(SectorPlanetRecord, capitol) == offsetof(RoverSectorPlanet, capitol));
static_assert(offsetof(SectorPlanetRecord, desc) == offsetof(RoverSectorPlanet, desc));
static_assert(offsetof(SectorPlanetRecord, ta_map) == offsetof(RoverSectorPlanet, ta_map));
static_assert(
    offsetof(SectorPlanetRecord, ta_mapsource) == offsetof(RoverSectorPlanet, ta_mapsource)
);
static_assert(offsetof(SectorPlanetRecord, ta_maptype) == offsetof(RoverSectorPlanet, ta_maptype));
static_assert(offsetof(SectorPlanetRecord, pan_gaf) == offsetof(RoverSectorPlanet, pan_gaf));
static_assert(offsetof(SectorPlanetRecord, pan_seq) == offsetof(RoverSectorPlanet, pan_seq));
static_assert(
    offsetof(SectorPlanetRecord, playerlimit) == offsetof(RoverSectorPlanet, playerlimit)
);
static_assert(offsetof(SectorPlanetRecord, watching) == offsetof(RoverSectorPlanet, watching));
static_assert(sizeof(ConnectorRecord) == sizeof(RoverSectorConnector));
static_assert(offsetof(ConnectorRecord, planet2) == offsetof(RoverSectorConnector, planet2_id));

RoverSession g_session{};
AdEventNote g_ad_event_note = nullptr;
void* g_ad_event_note_context = nullptr;

// Status, found and go copied out of the pump. Eight is enough for one pump; a ninth drops the
// oldest so a go is not lost behind a run of status records. The dialog drains this each pulse.
constexpr int32_t kMatchNoticeCapacity = 8;
MatchmakingNotice g_match_notices[kMatchNoticeCapacity]{};
int32_t g_match_notice_count = 0;

void clear_match_notices() {
    g_match_notice_count = 0;
}

void push_match_notice(const MatchmakingNotice& notice) {
    if (g_match_notice_count >= kMatchNoticeCapacity) {
        std::memmove(
            g_match_notices,
            g_match_notices + 1,
            sizeof(MatchmakingNotice) * static_cast<std::size_t>(kMatchNoticeCapacity - 1)
        );
        g_match_notice_count = kMatchNoticeCapacity - 1;
    }
    g_match_notices[g_match_notice_count] = notice;
    ++g_match_notice_count;
}

// One QuickPlay sweep can name every planet in a sector. 96 holds that burst; a further line
// drops the oldest so a found is not lost behind a run of scans. The console drains this each pulse.
constexpr int32_t kQuickNoticeCapacity = 96;
QuickPlayNotice g_quick_notices[kQuickNoticeCapacity]{};
int32_t g_quick_notice_count = 0;

void clear_quick_notices() {
    g_quick_notice_count = 0;
}

void push_quick_notice(const QuickPlayNotice& notice) {
    if (g_quick_notice_count >= kQuickNoticeCapacity) {
        std::memmove(
            g_quick_notices,
            g_quick_notices + 1,
            sizeof(QuickPlayNotice) * static_cast<std::size_t>(kQuickNoticeCapacity - 1)
        );
        g_quick_notice_count = kQuickNoticeCapacity - 1;
    }
    g_quick_notices[g_quick_notice_count] = notice;
    ++g_quick_notice_count;
}

// Stages the login progress callback reacts to.
enum class LoginStage : int32_t {
    resolving = 1, // the yard master's host is being looked up; the message is that host
    contact = 4,   // the server answered (or did not)
    clearance = 7, // the login request was relayed
    result = 8,    // login_confirmed arrived; detail_a is the user id, detail_b the area
};
constexpr int32_t kLoginAccepted = 0;

// Explanations of the login_confirmed results 2 through 8; any other
// failure gets kLoginFailedText.
constexpr int32_t kFirstExplainedResult = 2;
constexpr const char* kLoginResultTexts[] = {
    "The username provided was invalid (letters, numbers, and the underscore are acceptable).",
    "Another player is already using that (guest) username, choose another.",
    "The password you provided was not correct for the username.  Check the values and try again.",
    "Someone is already logged in as that (registered) username.",
    "The registered username provided was not found.",
    "Guest logins are currently disabled.",
    "Please choose another username and try again.",
};
constexpr int32_t kExplainedResultCount =
    static_cast<int32_t>(sizeof kLoginResultTexts / sizeof kLoginResultTexts[0]);
constexpr const char* kLoginFailedText = "Unable to login to Boneyards, try again later.";
constexpr const char* kContactText = "Contact Established";
constexpr const char* kNoContactText = "Server Not responding";
constexpr const char* kClearanceText = "Verifying Clearance";
constexpr const char* kTryAgainText = "Try a different server or come back later";
constexpr const char* kDossierFailedText = "Unable to access dossier, Login Aborted";

constexpr const char* kUnexpectedDisconnectText = "An unexpected network\ndisconnect has occurred.";
constexpr const char* kLoginServerUnreachableText =
    "Unable to connect to\nlogin server.\n\nPlease try again later.";
// The login console's message when the yard master cannot be reached. The host fills %s.
// The original client showed a message box for a connect that failed; a resolve that fails, or a
// connect that is refused or times out, had no such line, so this one names the host.
constexpr const char* kCouldNotReachText =
    "Could not reach %s. Check your connection, or try again later.";
constexpr const char* kBoneyardsUnreachableText =
    "Unable to connect to\nBoneyards server.\n\nPlease try again later.";
constexpr const char* kBattleDisconnectText =
    "Boneyards: WARNING! DISCONNECTED FROM THE BONEYARDS!";
constexpr int kBattleDisconnectRepeats = 3;

// The system alert fields a page and a notification carry.
constexpr const char* kAlertUserIdKey = "user_id";
constexpr const char* kNotificationTypeKey = "notification_type";

// A guest (no password) is known locally by the name with this prefix.
constexpr const char* kGuestNamePrefix = "?_";

// Dossier fields.
constexpr const char* kUserStatusKey = "USER_STATUS";
constexpr const char* kLoginNameKey = "LOGINNAME";
constexpr const char* kAllegianceKey = "ALLEGIANCE";
constexpr const char* kAllegiancePreviousKey = "ALLEGIANCE_PREVIOUS";
constexpr const char* kAllegianceLockedKey = "ALLEGIANCE_LOCKED";
constexpr char kAllegianceArm = 'A';
constexpr char kAllegianceCore = 'C';
constexpr char kAllegianceUnknown = 'U';
// While this bit of the user id (its top bit) is set the dossier's login
// name does not replace the local one.
constexpr uint32_t kKeepLocalNameBit = 0x80000000U;

void copy_bounded(char* out, std::size_t capacity, const char* text) {
    utf8::copy(out, capacity, text, 0);
}

void copy_name(char* out, std::size_t capacity, const char* text) {
    utf8::copy(out, capacity, text, kPlayerNameCharacters);
}

void publish(const RoverSession* s) {
    if (s->events.session_changed != nullptr)
        s->events.session_changed(s->events.user, &s->record);
}

void show_login_status(const RoverSession* s, const char* text) {
    if (s->events.login_status != nullptr)
        s->events.login_status(s->events.user, text);
}

void show_notice(const RoverSession* s, const char* text) {
    if (s->events.notice != nullptr)
        s->events.notice(s->events.user, text);
}

bool battle_active(const RoverSession* s) {
    return s->events.battle_active != nullptr && s->events.battle_active(s->events.user);
}

void ROVER_CALL on_dossier(bool ok, RoverPropertyBag* properties);
void drop_profile_requests();

/// Reads a side letter from a dossier string. Empty and any other word are not a side.
///
/// @param text field text; null is not a side
/// @return 'A', 'C' or 0
char side_letter(const char* text) {
    if (text == nullptr || text[0] == '\0')
        return '\0';
    switch (text[0]) {
    case 'A':
    case 'a':
        return kAllegianceArm;
    case 'C':
    case 'c':
        return kAllegianceCore;
    default:
        return '\0';
    }
}

/// Stores a side field. A missing field, or one that is not Arm or Core, stores `when_absent`.
///
/// @param properties dossier bag
/// @param key field name
/// @param[out] slot record letter
/// @param when_absent letter stored when the field is absent or not a side
void read_side_field(RoverPropertyBag* properties, const char* key, char* slot, char when_absent) {
    RoverString text{};
    char letter = '\0';
    if (rover_call_get_field_string(properties, key, &text))
        letter = side_letter(rover_string_text(&text));
    rover_string_release(&text, true);
    *slot = letter != '\0' ? letter : when_absent;
}

/// Takes the dossier fields the game keeps.
///
/// Records whether USER_STATUS is Admin or Beta Tester, the login name
/// (unless the user id says to keep the local one) and the allegiance letter.
/// A missing ALLEGIANCE is no side for this sector ('U'). ALLEGIANCE_PREVIOUS
/// is the last sector's side, or none. ALLEGIANCE_LOCKED is 0 or 1, and 0
/// when the dossier omits it. A missing status is an ordinary account. The
/// console stores that login name as the saved Username when the dossier arrives.
///
/// @param[in,out] s session whose record takes the fields
/// @param properties dossier bag
void read_dossier(RoverSession* s, RoverPropertyBag* properties) {
    int32_t user_status = 0;
    if (!rover_call_get_field_int(properties, kUserStatusKey, &user_status))
        user_status = 0;
    s->record.matchmaking_enabled = account_sees_unfinished(user_status) ? 1 : 0;
    RoverString login_name{};
    if (rover_call_get_field_string(properties, kLoginNameKey, &login_name) &&
        (s->record.user_id & kKeepLocalNameBit) == 0) {
        copy_name(s->record.user_name, sizeof s->record.user_name, rover_string_text(&login_name));
    }
    read_side_field(properties, kAllegianceKey, &s->record.allegiance, kAllegianceUnknown);
    read_side_field(properties, kAllegiancePreviousKey, &s->record.allegiance_previous, '\0');
    int32_t locked = 0;
    if (!rover_call_get_field_int(properties, kAllegianceLockedKey, &locked))
        locked = 0;
    s->record.allegiance_locked = locked != 0 ? 1 : 0;
    rover_string_release(&login_name, true);
}

/// Ends a login attempt.
///
/// The dialog gets LOGIN back, the session returns to an active client and
/// Rover restarts its session. Also the logout callback of an aborted login.
void ROVER_CALL reset_login() {
    rover_sector_fetch_cancel();
    RoverSession* s = &g_session;
    if (s->events.login_reset != nullptr)
        s->events.login_reset(s->events.user);
    s->record.matchmaking_enabled = 0;
    s->record.level = SessionLevel::client_active;
    publish(s);
    rover_call_reset_session();
}

/// Reports the login stages to the dialog.
///
/// A confirmed login stores the user and area ids, marks the session logged
/// in and asks for the dossier; any failure explains itself and ends the
/// attempt. The name typed stays until the dossier replaces it.
///
/// @param stage login stage (4 contact, 7 clearance, 8 result)
/// @param result 0 when the stage succeeded; at stage 8 the login_confirmed result
/// @param detail_a at stage 8, the user id
/// @param detail_b at stage 8, the area id
/// @param message server text for a failed contact; may be null
void ROVER_CALL on_login_progress(
    int32_t stage, int32_t result, int32_t detail_a, int32_t detail_b, const char* message
) {
    RoverSession* s = &g_session;
    switch (static_cast<LoginStage>(stage)) {
    case LoginStage::resolving:
        copy_bounded(s->yard_host, sizeof s->yard_host, message != nullptr ? message : "");
        return;
    case LoginStage::contact:
        if (result == kLoginAccepted) {
            show_login_status(s, kContactText);
            return;
        }
        if (message != nullptr) {
            show_login_status(s, message);
        } else {
            show_login_status(s, kNoContactText);
            show_login_status(s, kTryAgainText);
        }
        break;
    case LoginStage::clearance:
        if (result == kLoginAccepted) {
            show_login_status(s, kClearanceText);
            return;
        }
        show_login_status(s, kRelayFailedText);
        show_login_status(s, kTryAgainText);
        break;
    case LoginStage::result: {
        if (result == kLoginAccepted) {
            s->record.user_id = static_cast<uint32_t>(detail_a);
            s->record.area_id = static_cast<uint32_t>(detail_b);
            s->record.level = SessionLevel::logged_in;
            publish(s);
            rover_call_request_get_profile(s->record.user_id, on_dossier, nullptr);
            return;
        }
        const int32_t index = result - kFirstExplainedResult;
        show_login_status(
            s,
            index >= 0 && index < kExplainedResultCount ? kLoginResultTexts[index]
                                                        : kLoginFailedText
        );
        break;
    }
    default:
        return;
    }
    reset_login();
}

/// Takes the dossier requested after login.
///
/// Without it the login is aborted (logged out, then reset). With it the
/// dossier fields are read and, when the area name can be asked for, the
/// sector request is begun. The session stays logged in until the idle tick
/// settles that request. A disconnect or a reset during the wait cancels it.
/// The console's half, stopping the Main Frame's activity light and storing
/// BetaLoginUX, runs in the status widget's session event.
///
/// @param ok whether the dossier arrived
/// @param properties dossier bag
void ROVER_CALL on_dossier(bool ok, RoverPropertyBag* properties) {
    RoverSession* s = &g_session;
    if (!ok) {
        show_login_status(s, kDossierFailedText);
        rover_call_send_logout(static_cast<int32_t>(s->record.user_id), reset_login);
    } else {
        const char* area = rover_call_match_area_name();
        const bool waiting = rover_session_begin_sector_fetch(s->record.area_id, area);
        if (!waiting && s->record.level == SessionLevel::logged_in)
            s->record.level = SessionLevel::dossier_loaded;
    }
    read_dossier(s, properties);
    publish(s);
}

/// Hands a system alert to the Main Frame's alert handler with the field of its property bag its
/// kind reads.
///
/// A page reads its user_id (0 when the bag has none), a notification its
/// notification_type.
///
/// @param kind AlertKind value
/// @param text alert text
/// @param fields the alert's property bag
void ROVER_CALL on_system_alert(int32_t kind, const char* text, void* fields) {
    const RoverSession* s = &g_session;
    auto* bag = static_cast<RoverPropertyBag*>(fields);
    AlertReport alert{kind, text, 0, ""};
    RoverString notification_type{};
    if (kind == static_cast<int32_t>(AlertKind::page)) {
        (void)rover_call_get_field_uint(bag, kAlertUserIdKey, &alert.sender_id);
    } else if (
        kind == static_cast<int32_t>(AlertKind::notification) &&
        rover_call_get_field_string(bag, kNotificationTypeKey, &notification_type)
    ) {
        alert.notification_type = rover_string_text(&notification_type);
    }
    if (s->events.system_alert != nullptr)
        s->events.system_alert(s->events.user, &alert);
    rover_string_release(&notification_type, true);
}

/// The message box for a yard master that could not be reached, naming the host when the resolve
/// step recorded one.
///
/// @param s session whose yard_host is the host, or empty
void show_yard_unreachable(const RoverSession* s) {
    if (s->yard_host[0] == '\0') {
        show_notice(s, kLoginServerUnreachableText);
        return;
    }
    char pattern[160];
    copy_shown_pattern(pattern, sizeof pattern, kCouldNotReachText);
    char text[320];
    const int written = std::snprintf(text, sizeof text, pattern, s->yard_host);
    if (written < 0)
        text[0] = '\0';
    show_notice(s, text[0] != '\0' ? text : kLoginServerUnreachableText);
}

/// Reports a dropped or failed connection.
///
/// A link that was no longer connecting (an open link dropped) marks the
/// session disconnected. During a Boneyards battle the warning goes to the
/// battle's message area; otherwise the Main Frame's pending notice names
/// the failure, and dismissing that notice returns to the main menu. The
/// failure is the drop, or, while still connecting, the login server
/// (nonzero `which`) or the Boneyards server. The core state flag raised
/// with the battle warning is not set.
///
/// @param which while still connecting, nonzero for the login server (low byte read)
/// @param connecting zero when an open link dropped
/// @param socket_error not read
void ROVER_CALL on_disconnect(int32_t which, char connecting, int32_t /*socket_error*/) {
    rover_sector_fetch_cancel();
    RoverSession* s = &g_session;
    if (connecting == 0) {
        s->record.matchmaking_enabled = 0;
        s->record.level = SessionLevel::disconnected;
        publish(s);
    }
    if (battle_active(s)) {
        for (int i = 0; i < kBattleDisconnectRepeats; ++i)
            if (s->events.battle_message != nullptr)
                s->events.battle_message(s->events.user, kBattleDisconnectText);
        return;
    }
    if (connecting == 0) {
        show_notice(s, kUnexpectedDisconnectText);
        return;
    }
    if (static_cast<uint8_t>(which) != 0)
        show_yard_unreachable(s);
    else
        show_notice(s, kBoneyardsUnreachableText);
    // LOGIN was held down for this attempt. Hand it back once the box is up.
    if (s->record.level == SessionLevel::client_active)
        reset_login();
}

// Leaves Central Command for good: stops Rover's workers, detaches, releases
// the table and marks the session offline. Also the logout callback of a
// close while logged in.
void ROVER_CALL release_client() {
    RoverSession* s = &g_session;
    rover_call_deactivate();
    rover_call_detach();
    rover_module_close();
    drop_profile_requests();
    rover_matchmaking_set_handlers(nullptr, nullptr, nullptr);
    clear_match_notices();
    rover_quickplay_set_handlers(nullptr, nullptr, nullptr);
    clear_quick_notices();
    s->pump_enabled = 0;
    s->record.level = SessionLevel::offline;
    publish(s);
}

/// True when an interface other than the loopback is up.
///
/// Without one, Enter Boneyards shows "Please connect to the Internet before
/// entering the Boneyards" and does not open the login console.
bool service_network_available(void*) {
    return network_link_available();
}

// Loads, attaches and activates the client module; the session then pumps.
ActivationResult service_activate(void* user) {
    auto* s = static_cast<RoverSession*>(user);
    if (!rover_module_load())
        return ActivationResult::module_missing;
    if (!rover_call_attach())
        return ActivationResult::attach_failed;
    if (!rover_call_activate())
        return ActivationResult::activate_failed;
    s->record.level = SessionLevel::client_active;
    s->pump_enabled = 1;
    publish(s);
    return ActivationResult::ok;
}

/// Logs out first while logged in and releases the client once the logout is confirmed; otherwise
/// releases it at once.
///
/// @param user the RoverSession
void service_close(void* user) {
    auto* s = static_cast<RoverSession*>(user);
    if (s->record.level >= SessionLevel::logged_in) {
        rover_call_send_logout(static_cast<int32_t>(s->record.user_id), release_client);
        return;
    }
    release_client();
}

bool service_connected(void* user) {
    return static_cast<const RoverSession*>(user)->record.level >= SessionLevel::logged_in;
}

// The Central Command half of the dialog's LOGIN: the account goes to Rover
// with the three callbacks; a guest is then known by the prefixed name.
ServiceResult service_login(void* user, const LoginRequest* request) {
    auto* s = static_cast<RoverSession*>(user);
    s->yard_host[0] = '\0';
    copy_name(s->record.user_name, sizeof s->record.user_name, request->user_name);
    copy_name(s->password, sizeof s->password, request->password);
    const char* server = request->server_name != nullptr ? request->server_name : "";
    (void)rover_call_start_login(
        server, s->record.user_name, s->password, on_login_progress, on_system_alert, on_disconnect
    );
    if (s->password[0] == '\0') {
        char guest[kSessionNameCapacity * 2];
        copy_bounded(guest, sizeof guest, kGuestNamePrefix);
        const std::size_t prefix = std::strlen(guest);
        copy_bounded(guest + prefix, sizeof guest - prefix, request->user_name);
        copy_name(s->record.user_name, sizeof s->record.user_name, guest);
    }
    return ServiceResult::pending;
}

// The Central Command half of the dialog's CANCEL: a confirmed login is
// logged out (then reset), anything else is reset at once.
void service_cancel_login(void* user) {
    const auto* s = static_cast<const RoverSession*>(user);
    if (s->record.level == SessionLevel::logged_in)
        rover_call_send_logout(static_cast<int32_t>(s->record.user_id), reset_login);
    else
        reset_login();
}

// Leaves the current server. A session that is logged in, dossier included,
// sends the logout and the reset runs when that finishes. Anything earlier
// resets at once, which is also how a transfer logs in again.
void service_logout(void* user) {
    auto* s = static_cast<RoverSession*>(user);
    if (s->record.level >= SessionLevel::logged_in)
        rover_call_send_logout(static_cast<int32_t>(s->record.user_id), reset_login);
    else
        reset_login();
}

void service_pump(void* user) {
    rover_session_idle(static_cast<const RoverSession*>(user));
}

void (*g_server_entry)(void*, const char*){};
void* g_server_context{};

void ROVER_CALL service_server_entry(const char* location) {
    if (g_server_entry != nullptr)
        g_server_entry(g_server_context, location);
}

void service_list_servers(void*, void (*entry)(void*, const char*), void* context) {
    g_server_entry = entry;
    g_server_context = context;
    rover_call_enumerate_areas(service_server_entry);
    g_server_entry = nullptr;
    g_server_context = nullptr;
}

const char* service_current_server(void*) {
    return rover_session_selected_server();
}

const char* service_server_region(void*, const char* location) {
    return rover_server_region(location);
}

void service_set_events(void* user, const ServiceEvents* events) {
    static_cast<RoverSession*>(user)->events = events != nullptr ? *events : ServiceEvents{};
}

// Rover's ends of the war-map feed callbacks the galaxy load and a hotspot
// press hand slots 38 and 39; the screens apply them.
void ROVER_CALL on_allegiances(const char* allegiances) {
    const RoverSession* s = &g_session;
    if (s->events.allegiances != nullptr)
        s->events.allegiances(s->events.user, allegiances);
}

void ROVER_CALL on_activity(const char* digits) {
    const RoverSession* s = &g_session;
    if (s->events.activity != nullptr)
        s->events.activity(s->events.user, digits);
}

void ROVER_CALL on_census(
    uint32_t area_id,
    int32_t population,
    int32_t activity,
    int32_t in_progress,
    int32_t forming,
    int32_t core_wins,
    int32_t arm_wins
) {
    const RoverSession* s = &g_session;
    const CensusReport report{
        area_id, population, activity, in_progress, forming, core_wins, arm_wins
    };
    if (s->events.census != nullptr)
        s->events.census(s->events.user, &report);
}

// Rover's ends of the chat room callbacks the game hands slots 9 to 12 and
// 69; the screens apply them.
void ROVER_CALL on_room_listed(
    int32_t change,
    int32_t area_id,
    int32_t room_id,
    const char* name,
    const char* description,
    bool needs_password,
    int32_t users,
    int32_t type,
    const char* tag,
    bool last
) {
    const RoverSession* s = &g_session;
    int32_t global = 0;
    const bool global_known = rover_take_room_global(&global);
    if (s->events.room_listed != nullptr)
        s->events.room_listed(
            s->events.user,
            change,
            static_cast<uint32_t>(area_id),
            static_cast<uint32_t>(room_id),
            name,
            description,
            needs_password,
            users,
            type,
            tag,
            last,
            global_known,
            global
        );
}

void ROVER_CALL on_room_updated(
    int32_t change,
    int32_t area_id,
    int32_t room_id,
    const char* name,
    const char* description,
    bool needs_password,
    int32_t users,
    int32_t type,
    const char* tag
) {
    const RoverSession* s = &g_session;
    int32_t global = 0;
    (void)rover_take_room_global(&global);
    if (s->events.room_updated != nullptr)
        s->events.room_updated(
            s->events.user,
            change,
            static_cast<uint32_t>(area_id),
            static_cast<uint32_t>(room_id),
            name,
            description,
            needs_password,
            users,
            type,
            tag
        );
}

void ROVER_CALL on_room_entered(int32_t result) {
    const RoverSession* s = &g_session;
    if (s->events.room_entered != nullptr)
        s->events.room_entered(s->events.user, result);
}

void ROVER_CALL on_room_member(
    int32_t change,
    int32_t user_id,
    const char* name,
    int32_t latency,
    int32_t icon,
    int32_t rank,
    const char* allegiance
) {
    const RoverSession* s = &g_session;
    if (s->events.room_member != nullptr)
        s->events.room_member(
            s->events.user,
            change,
            static_cast<uint32_t>(user_id),
            name,
            latency,
            icon,
            rank,
            allegiance
        );
}

void ROVER_CALL on_chat_message(int32_t no_user_id, int32_t user_id, const char* text) {
    const RoverSession* s = &g_session;
    if (s->events.chat_message != nullptr)
        s->events.chat_message(
            s->events.user, no_user_id != 0, static_cast<uint32_t>(user_id), text
        );
}

void ROVER_CALL on_room_left(bool failed) {
    const RoverSession* s = &g_session;
    if (s->events.room_left != nullptr)
        s->events.room_left(s->events.user, failed);
}

void ROVER_CALL on_room_created(uint32_t* area_id, uint32_t* room_id, const char* name) {
    const RoverSession* s = &g_session;
    if (s->events.room_created != nullptr)
        s->events.room_created(s->events.user, *area_id, *room_id, name);
}

// Rover's ends of the battle callbacks the game hands slots 15 to 23: the
// done, battle, intel, intel reply and report callbacks of the battle
// console, and the launch and debriefing callbacks of a battle hosted or
// joined. The screens apply them.
void ROVER_CALL on_battles_done(bool failed) {
    const RoverSession* s = &g_session;
    if (s->events.battles_done != nullptr)
        s->events.battles_done(s->events.user, failed);
}

void ROVER_CALL on_battle_info(
    int32_t change,
    const char* game_id,
    const char* name,
    int32_t type,
    int32_t forming,
    int32_t host_code
) {
    const RoverSession* s = &g_session;
    if (s->events.battle_info != nullptr)
        s->events.battle_info(s->events.user, change, game_id, name, type, forming, host_code);
}

void ROVER_CALL on_intel_message(int32_t no_game_id, const char* game_id, const char* message) {
    const RoverSession* s = &g_session;
    if (s->events.intel != nullptr)
        s->events.intel(s->events.user, no_game_id != 0 ? nullptr : game_id, message);
}

void ROVER_CALL on_battle_intel(
    const char* game_id,
    uint32_t user_id,
    const char* user_name,
    int32_t has_password,
    const char* mission,
    int32_t skill,
    int32_t players,
    int32_t on_death,
    bool failed
) {
    const RoverSession* s = &g_session;
    const BattleIntel intel{
        game_id, user_id, user_name, has_password, mission, skill, players, on_death, failed
    };
    if (s->events.battle_intel != nullptr)
        s->events.battle_intel(s->events.user, &intel);
}

void ROVER_CALL on_battle_report(
    const char* game_id,
    uint32_t user_id,
    int32_t icon,
    const char* user_name,
    int32_t kills,
    int32_t losses,
    int32_t energy,
    int32_t metal,
    int32_t score,
    const char* message
) {
    const RoverSession* s = &g_session;
    const BattleReport report{
        game_id, user_id, icon, user_name, kills, losses, energy, metal, score, message
    };
    if (s->events.battle_report != nullptr)
        s->events.battle_report(s->events.user, &report);
}

/// Reads the planet from a launch_game record.
///
/// `area_id` is a type-U field, hexadecimal, as rover_hex_value reads it.
/// The field may sit before or after the command. A record that is not a
/// launch, or a launch without the field, is planet 0.
///
/// @param wire the launch_game record text; it need not be terminated
/// @return the planet, or 0 when the record does not name one
uint32_t launch_wire_area(std::string_view wire) {
    constexpr std::string_view command = "command=launch_game";
    const std::size_t command_at = wire.find(command);
    if (command_at == std::string_view::npos)
        return 0;
    const std::size_t entry_at = wire.rfind('[', command_at);
    const std::size_t entry_begin = entry_at == std::string_view::npos ? 0 : entry_at;
    const std::size_t entry_end_at = wire.find(']', command_at);
    const std::size_t entry_end =
        entry_end_at == std::string_view::npos ? wire.size() : entry_end_at;
    const std::string_view launch_entry = wire.substr(entry_begin, entry_end - entry_begin);
    constexpr std::string_view key = "U:area_id=";
    const std::size_t key_at = launch_entry.find(key);
    if (key_at == std::string_view::npos)
        return 0;
    const std::string_view raw = launch_entry.substr(key_at + key.size());
    char value_text[48];
    std::size_t copied = 0;
    for (const char byte : raw) {
        if (byte == '>' || copied + 1 >= sizeof value_text)
            break;
        value_text[copied++] = byte;
    }
    value_text[copied] = '\0';
    return rover_hex_value(value_text);
}

void ROVER_CALL on_launch(
    const char* game_id,
    const char* session,
    const char* address,
    const char* password,
    bool creator,
    bool lock_options,
    const char* mission,
    int32_t playerlimit,
    int32_t deathopt,
    int32_t energy,
    int32_t metal,
    int32_t location,
    int32_t los,
    int32_t mapping,
    int32_t maxunits,
    int32_t watching,
    int32_t cheats,
    int32_t tournament,
    const char* allegiance,
    int32_t team,
    int32_t result
) {
    const RoverSession* s = &g_session;
    int32_t wire_length = 0;
    const char* wire = rover_launch_wire(&wire_length);
    const std::string_view wire_text =
        wire != nullptr && wire_length > 0
            ? std::string_view(wire, static_cast<std::size_t>(wire_length))
            : std::string_view{};
    const uint32_t area_id = launch_wire_area(wire_text);
    const LaunchReport launch{game_id,    session,     address,  password, creator, lock_options,
                              mission,    playerlimit, deathopt, energy,   metal,   location,
                              los,        mapping,     maxunits, watching, cheats,  tournament,
                              allegiance, team,        result,   area_id};
    std::optional<OverlayLaunch> overlay;
    if (!wire_text.empty())
        overlay = parse_overlay_launch(wire_text);
    set_current_overlay_launch(overlay.has_value() ? &(*overlay) : nullptr);
    if (s->events.launch != nullptr)
        s->events.launch(s->events.user, &launch);
    set_current_overlay_launch(nullptr);
}

void ROVER_CALL on_debriefing(uint32_t area_id, uint32_t room_id, bool first) {
    const RoverSession* s = &g_session;
    if (s->events.debriefing != nullptr)
        s->events.debriefing(s->events.user, area_id, room_id, first);
}

// A request Rover accepted is answered through its callbacks.
ServiceResult accepted(bool queued) {
    return queued ? ServiceResult::pending : ServiceResult::failed;
}

// The slots that return nothing queue whenever the table is bound.
ServiceResult table_bound() {
    return accepted(rover_module_state()->table != nullptr);
}

ServiceResult service_request_room_list(void*, uint32_t area_id) {
    return accepted(rover_call_get_rooms(static_cast<int32_t>(area_id), on_room_listed));
}

ServiceResult
service_join_room(void*, uint32_t area_id, uint32_t room_id, const char* password, const char*) {
    return accepted(rover_call_enter_room(
        static_cast<int32_t>(room_id),
        static_cast<int32_t>(area_id),
        password,
        on_room_entered,
        on_room_member,
        on_room_updated,
        on_chat_message
    ));
}

ServiceResult service_switch_room(
    void*,
    uint32_t area_id,
    uint32_t room_id,
    const char* password,
    uint32_t old_area_id,
    uint32_t old_room_id,
    const char*
) {
    return accepted(rover_call_switch_room(
        static_cast<int32_t>(room_id),
        static_cast<int32_t>(area_id),
        password,
        static_cast<int32_t>(old_room_id),
        static_cast<int32_t>(old_area_id),
        on_room_entered,
        on_room_member,
        on_room_updated,
        on_chat_message
    ));
}

ServiceResult service_leave_room(void*, uint32_t area_id, uint32_t room_id) {
    return accepted(rover_call_leave_room(
        static_cast<int32_t>(room_id), static_cast<int32_t>(area_id), on_room_left
    ));
}

ServiceResult service_send_chat(
    void*, uint32_t room_id, uint32_t area_id, uint32_t target_user, const char* text
) {
    return accepted(rover_call_send_chat(
        static_cast<int32_t>(room_id),
        static_cast<int32_t>(area_id),
        static_cast<int32_t>(target_user),
        text
    ));
}

ServiceResult service_page_user(void*, const char* user_name, const char* text) {
    rover_call_send_page_by_name(user_name, text);
    return table_bound();
}

ServiceResult service_create_room(
    void*, uint32_t area_id, const char* name, const char* description, const char* password
) {
    rover_call_create_room(area_id, name, description, password, on_room_created);
    return table_bound();
}

ServiceResult service_enter_battles(void*, uint32_t area_id) {
    return accepted(
        rover_call_enter_battles(area_id, on_battles_done, on_battle_info, on_intel_message)
    );
}

ServiceResult service_leave_battles(void*, uint32_t area_id) {
    return accepted(rover_call_leave_battles(area_id, on_battles_done));
}

ServiceResult service_get_battle(void*, uint32_t area_id, const char* game_id) {
    return accepted(rover_call_get_battle(area_id, game_id, on_battle_intel, on_battle_report));
}

// The Host panel's START: no description or keyword.
ServiceResult service_host_game(void*, const HostGameRequest* request) {
    return accepted(rover_call_host_game(
        request->area_id,
        request->game_mode,
        request->name,
        request->password,
        nullptr,
        nullptr,
        request->skill,
        request->players,
        request->commander_death,
        on_launch,
        on_debriefing
    ));
}

ServiceResult
service_join_game(void*, uint32_t area_id, const char* game_id, const char* password) {
    return accepted(rover_call_join_game(area_id, game_id, password, on_launch, on_debriefing));
}

const char* service_area_name(void*) {
    return rover_call_match_area_name();
}

// The record reader opens a relative path through the placed platform, which
// finds the player's copy, then the game's files, then the install. The
// caller's path is handed on as named; null is an empty path.
SectorMapReader* service_open_sector_map(void*, const char* path, SectorMapHeader* header) {
    const char* named = path != nullptr ? path : "";
    return reinterpret_cast<SectorMapReader*>(rover_call_open_sector_map(
        named,
        &header->sector_id,
        header->sector_name,
        &header->width,
        &header->height,
        &header->planet_count,
        &header->connector_count
    ));
}

bool service_read_sector_map(
    void*,
    SectorMapReader* reader,
    int32_t width,
    int32_t height,
    SectorPlanetRecord* planets,
    int32_t planet_count,
    ConnectorRecord* connectors,
    int32_t connector_count
) {
    return rover_call_read_sector_planets(
        reinterpret_cast<RoverSectorReader*>(reader),
        width,
        height,
        reinterpret_cast<RoverSectorPlanet*>(planets),
        planet_count,
        reinterpret_cast<RoverSectorConnector*>(connectors),
        connector_count
    );
}

void service_close_sector_map(void*, SectorMapReader* reader) {
    rover_call_close_sector_map(reinterpret_cast<RoverSectorReader*>(reader));
}

void service_request_allegiance(void*) {
    rover_call_request_allegiance(on_allegiances);
}

void service_request_census(void*, uint32_t area_id) {
    rover_call_request_census(area_id, on_census, on_activity);
}

const ProfileTemplates* service_profile_templates(void*) {
    return profile_templates_load();
}

// Slot 40's content server of a kind. The game presets the record's type to
// the kind it asks for; an invalid type back means no server is known.
bool service_content_server(
    void*, int32_t server_type, char* host, std::size_t host_capacity, uint16_t* port
) {
    if (rover_module_state()->table == nullptr)
        return false;
    RoverFileServer server{};
    server.type_index = server_type;
    rover_call_get_file_server(server_type, &server);
    if (server.type_index == OA_ROVER_FILE_SERVER_INVALID)
        return false;
    server.host[sizeof server.host - 1] = '\0';
    copy_bounded(host, host_capacity, server.host);
    *port = server.port;
    return true;
}

// Downloaded documents are saved to files named as GetTempFileName names
// them in the temporary directory: the prefix, a number of up to four hex
// digits not yet taken there, and .TMP. The contents consoles delete a file
// whose path holds the prefix once they have read it.
constexpr const char* kTempFilePrefix = "0BY";
constexpr uint32_t kTempFileNumbers = 0xffff;

// Creates the next free temporary download file, empty, and writes its path.
bool create_temp_file(char* out, std::size_t capacity) {
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    if (error)
        return false;
    static uint32_t next = static_cast<uint32_t>(std::time(nullptr));
    for (uint32_t tried = 0; tried < kTempFileNumbers; ++tried) {
        char name[0x10];
        const uint32_t number = next++ % kTempFileNumbers + 1;
        std::snprintf(name, sizeof name, "%s%X.TMP", kTempFilePrefix, number);
        const std::string path = (directory / name).string();
        std::FILE* file = oa::platform::open_file(path.c_str(), "wx");
        if (file == nullptr) {
            if (errno == EEXIST)
                continue;
            return false;
        }
        std::fclose(file);
        if (path.size() >= capacity) {
            std::remove(path.c_str());
            return false;
        }
        copy_bounded(out, capacity, path.c_str());
        return true;
    }
    return false;
}

// Rover's end of the completion callback the game hands slot 32: the
// screens take the arrived document.
bool ROVER_CALL on_file_fetched(const char* url, const char* file, int32_t status) {
    const RoverSession* s = &g_session;
    if (s->events.file_arrived != nullptr)
        s->events.file_arrived(s->events.user, url, file, status);
    return true;
}

// Slot 32 with the completion callback, saving into a new temporary file.
ServiceResult service_fetch_file(void*, const char* url, char* dest_path, std::size_t capacity) {
    if (capacity != 0)
        dest_path[0] = '\0';
    if (rover_module_state()->table == nullptr)
        return ServiceResult::not_connected;
    if (!create_temp_file(dest_path, capacity))
        return ServiceResult::failed;
    rover_call_fetch_http_file(url, dest_path, on_file_fetched, nullptr);
    return ServiceResult::pending;
}

// Fields of the locator's answer that the game's locator callback reads
// from its bag before it formats the line.
constexpr const char* kLocateAreaIdKey = "area_id";
constexpr const char* kLocateRoomIdKey = "room_id";
constexpr const char* kLocateGameIdKey = "game_id";
constexpr const char* kLocatePassRequiredKey = "pass_req";
constexpr const char* kLocateGameNameKey = "game_name";
constexpr const char* kLocateAreaNameKey = "area_name";
constexpr const char* kLocateRoomNameKey = "room_name";
constexpr const char* kLocateServerNameKey = "server_name";

// Where the locator's answer goes. The game prints it on its one chat console;
// the native console passes itself as the context.
LocateCallback g_locate_callback{};
void* g_locate_context{};

void read_bag_text(RoverPropertyBag* bag, const char* key, char* out, std::size_t capacity) {
    RoverString value{};
    if (rover_call_get_field_string(bag, key, &value))
        copy_bounded(out, capacity, rover_string_text(&value));
    rover_string_release(&value, true);
}

// Rover's end of the locator callback: the answer's code and name, and the
// numbers (0 when absent) and texts (empty when absent) the game reads from
// its bag; the user id and icon are not used.
void ROVER_CALL
on_user_located(int32_t loc_code, uint32_t, const char* name, int32_t, RoverPropertyBag* fields) {
    LocateReport report{};
    report.status = loc_code;
    copy_bounded(report.user_name, sizeof report.user_name, name);
    rover_call_get_field_uint(fields, kLocateAreaIdKey, &report.area_id);
    rover_call_get_field_uint(fields, kLocateRoomIdKey, &report.room_id);
    read_bag_text(fields, kLocateGameIdKey, report.game_id, sizeof report.game_id);
    int32_t pass_required = 0;
    rover_call_get_field_int(fields, kLocatePassRequiredKey, &pass_required);
    report.pass_required = static_cast<uint32_t>(pass_required);
    read_bag_text(fields, kLocateGameNameKey, report.game_name, sizeof report.game_name);
    read_bag_text(fields, kLocateAreaNameKey, report.area_name, sizeof report.area_name);
    read_bag_text(fields, kLocateRoomNameKey, report.room_name, sizeof report.room_name);
    read_bag_text(fields, kLocateServerNameKey, report.server_name, sizeof report.server_name);
    if (g_locate_callback != nullptr)
        g_locate_callback(g_locate_context, &report);
}

// The chat console's +locate: slot 65 with the locator callback; the answer
// arrives through the pump.
ServiceResult
service_locate_user(void*, const char* user_name, LocateCallback callback, void* context) {
    g_locate_callback = callback;
    g_locate_context = context;
    rover_call_locate_player(user_name, on_user_located);
    return ServiceResult::pending;
}

// Where the answers to a battle search go. The Locator passes itself as the
// context; a new search replaces the last one's.
GameLocateCallback g_game_locate_callback{};
void* g_game_locate_context{};

// Rover's end of the game-locate callback: one battle whose name holds the
// text asked for. An answer with no game id is the closing row of a search
// that matched nothing.
void ROVER_CALL on_game_located(
    uint32_t area_id,
    const char* game_id,
    const char* name,
    int32_t type,
    int32_t forming,
    int32_t pass_required,
    int32_t last
) {
    GameLocateReport report{};
    report.area_id = area_id;
    copy_bounded(report.game_id, sizeof report.game_id, game_id);
    copy_bounded(report.name, sizeof report.name, name);
    report.type = type;
    report.forming = forming != 0;
    report.pass_required = pass_required != 0;
    report.last = last != 0;
    int32_t players = 0;
    int32_t player_limit = 0;
    report.seats_known = rover_take_game_seats(&players, &player_limit);
    if (report.seats_known) {
        report.players = players;
        report.playerlimit = player_limit;
    }
    const GameLocateCallback callback = g_game_locate_callback;
    void* context = g_game_locate_context;
    if (report.last) {
        g_game_locate_callback = nullptr;
        g_game_locate_context = nullptr;
    }
    if (callback != nullptr)
        callback(context, &report);
}

// The Locator's Find Battle: slot 66 with the game-locate callback; the
// answers arrive through the pump.
ServiceResult
service_locate_game(void*, const char* name, GameLocateCallback callback, void* context) {
    if (rover_module_state()->table == nullptr)
        return ServiceResult::not_connected;
    g_game_locate_callback = callback;
    g_game_locate_context = context;
    rover_call_locate_game(name, on_game_located);
    return ServiceResult::pending;
}

// Profile requests awaiting their replies. Rover's profile callbacks carry
// no context, so a reply finds its request by the user id among its fields;
// one without (a failed reply may have none) belongs to the latest request,
// the one Rover answers. A new request for a user replaces the last one's.
struct ProfileRequest {
    uint32_t user_id;
    ProfileCallback on_profile;
    LadderCallback on_ladder;
    void* context;
    uint32_t order; // 0 while the entry is free
};

constexpr std::size_t kProfileRequestCapacity = 4;
constexpr const char* kProfileUserIdKey = "user_id";
ProfileRequest g_profile_requests[kProfileRequestCapacity]{};
uint32_t g_profile_order = 0;

ProfileRequest* profile_request_slot(uint32_t user_id) {
    ProfileRequest* oldest = &g_profile_requests[0];
    for (ProfileRequest& request : g_profile_requests) {
        if (request.order != 0 && request.user_id == user_id)
            return &request;
        if (request.order < oldest->order)
            oldest = &request;
    }
    return oldest;
}

ProfileRequest* profile_request_for(RoverPropertyBag* fields) {
    uint32_t user_id = 0;
    int32_t signed_id = 0;
    bool named = rover_call_get_field_uint(fields, kProfileUserIdKey, &user_id);
    if (!named && rover_call_get_field_int(fields, kProfileUserIdKey, &signed_id)) {
        user_id = static_cast<uint32_t>(signed_id);
        named = true;
    }
    ProfileRequest* latest = nullptr;
    for (ProfileRequest& request : g_profile_requests) {
        if (request.order == 0)
            continue;
        if (named && request.user_id == user_id)
            return &request;
        if (latest == nullptr || request.order > latest->order)
            latest = &request;
    }
    return named ? nullptr : latest;
}

// A Rover bag read and written through the keyed bag calls.
bool bag_get_string(void* bag, const char* key, char* out, std::size_t capacity) {
    RoverString value{};
    const bool found =
        rover_call_get_field_string(static_cast<RoverPropertyBag*>(bag), key, &value);
    if (found)
        copy_bounded(out, capacity, rover_string_text(&value));
    rover_string_release(&value, true);
    return found;
}

bool bag_get_int(void* bag, const char* key, int32_t* out) {
    return rover_call_get_field_int(static_cast<RoverPropertyBag*>(bag), key, out);
}

bool bag_get_uint(void* bag, const char* key, uint32_t* out) {
    return rover_call_get_field_uint(static_cast<RoverPropertyBag*>(bag), key, out);
}

void bag_set_string(void* bag, const char* key, const char* text) {
    RoverString value{};
    rover_string_assign(&value, text, static_cast<uint32_t>(std::strlen(text)));
    rover_call_set_bag_string(static_cast<RoverPropertyBag*>(bag), key, &value);
    rover_string_release(&value, true);
}

screens::ProfileFields bag_fields(RoverPropertyBag* bag) {
    return screens::ProfileFields{bag, bag_get_string, bag_get_int, bag_get_uint, bag_set_string};
}

// Rover's end of the profile callback slot 63 takes: the request is
// answered, and its entry freed before its callback runs. The player's own
// profile also refreshes the session's dossier fields, as reading a record
// does in 3.1c.
void ROVER_CALL on_profile_reply(bool ok, RoverPropertyBag* properties) {
    ProfileRequest* entry = profile_request_for(properties);
    if (entry == nullptr)
        return;
    RoverSession* s = &g_session;
    uint32_t user_id = 0;
    if (ok && rover_call_get_field_uint(properties, kProfileUserIdKey, &user_id) &&
        user_id == s->record.user_id) {
        read_dossier(s, properties);
        publish(s);
    }
    const ProfileRequest request = *entry;
    *entry = ProfileRequest{};
    const screens::ProfileFields fields = bag_fields(properties);
    if (request.on_profile != nullptr)
        request.on_profile(request.context, ok, &fields);
}

void ROVER_CALL
on_ladder_reply(int32_t ladder_id, int32_t ladder_subid, RoverPropertyBag* properties) {
    const ProfileRequest* request = profile_request_for(properties);
    if (request == nullptr || request->on_ladder == nullptr)
        return;
    const screens::ProfileFields fields = bag_fields(properties);
    request->on_ladder(request->context, ladder_id, ladder_subid, &fields);
}

// The chat console's player pane and the Profile console: slot 63 with the
// two reply callbacks; the replies arrive through the pump.
ServiceResult service_request_profile(
    void*, uint32_t user_id, ProfileCallback on_profile, LadderCallback on_ladder, void* context
) {
    ProfileRequest* request = profile_request_slot(user_id);
    *request = ProfileRequest{user_id, on_profile, on_ladder, context, ++g_profile_order};
    rover_call_request_get_profile(user_id, on_profile_reply, on_ladder_reply);
    return table_bound();
}

// The profile change awaiting its answer; Rover's callback carries no
// context, and a new change replaces the last one's.
struct ProfileUpdate {
    ProfileUpdatedCallback updated;
    void* context;
};

ProfileUpdate g_profile_update{};

void ROVER_CALL on_profile_updated(bool ok) {
    const ProfileUpdate update = g_profile_update;
    g_profile_update = ProfileUpdate{};
    if (update.updated != nullptr)
        update.updated(update.context, ok);
}

// The Profile console's APPLY: one property bag of the changed fields sent
// with slot 64 when it holds any, then destroyed.
ServiceResult service_submit_profile(
    void*,
    uint32_t user_id,
    const ProfileEdits* edits,
    ProfileUpdatedCallback updated,
    void* context
) {
    RoverPropertyBag* bag = rover_call_create_property_bag();
    if (bag == nullptr)
        return ServiceResult::failed;
    for (std::size_t i = 0; i < edits->count && i < kProfileEditCapacity; ++i)
        bag_set_string(bag, edits->fields[i].key, edits->fields[i].value);
    ServiceResult result = ServiceResult::failed;
    if (rover_call_get_first_bag_entry(bag) != nullptr) {
        g_profile_update = ProfileUpdate{updated, context};
        rover_call_request_set_profile(user_id, bag, on_profile_updated);
        result = ServiceResult::pending;
    }
    rover_call_destroy_property_bag(bag);
    return result;
}

void drop_profile_requests() {
    for (ProfileRequest& request : g_profile_requests)
        request = ProfileRequest{};
    g_profile_order = 0;
    g_profile_update = ProfileUpdate{};
}

bool service_peoplelist_contains(void* user, const char* list, uint32_t user_id) {
    PeopleLists* lists = rover_session_people_lists(static_cast<RoverSession*>(user));
    return lists != nullptr && people_list_contains(lists, list, static_cast<int32_t>(user_id));
}

void service_peoplelist_insert(void* user, const char* list, uint32_t user_id) {
    if (PeopleLists* lists = rover_session_people_lists(static_cast<RoverSession*>(user)))
        people_list_insert(lists, list, static_cast<int32_t>(user_id));
}

void service_peoplelist_erase(void* user, const char* list, uint32_t user_id) {
    if (PeopleLists* lists = rover_session_people_lists(static_cast<RoverSession*>(user)))
        (void)people_list_erase(lists, list, static_cast<int32_t>(user_id));
}

void service_peoplelist_release(void* user) {
    rover_session_release_people_lists(static_cast<RoverSession*>(user));
}

void copy_match_id(char* destination, std::size_t capacity, const char* source) {
    if (destination == nullptr || capacity == 0)
        return;
    std::snprintf(destination, capacity, "%s", source != nullptr ? source : "");
}

void on_matchmaking_status(const RoverMatchmakingStatus* status) {
    if (status == nullptr)
        return;
    MatchmakingNotice notice{};
    notice.kind = MatchmakingNoticeKind::status;
    notice.status.result = status->result;
    notice.status.queued = status->queued;
    notice.status.count = status->count;
    notice.status.meter = status->meter;
    push_match_notice(notice);
}

void on_matchmaking_found(const RoverMatchmakingFound* found) {
    if (found == nullptr)
        return;
    MatchmakingNotice notice{};
    notice.kind = MatchmakingNoticeKind::found;
    copy_match_id(notice.found.offer_id, sizeof notice.found.offer_id, found->offer_id);
    copy_match_id(notice.found.name, sizeof notice.found.name, found->name);
    notice.found.skill = found->skill;
    notice.found.game_type = found->game_type;
    notice.found.role = found->role;
    notice.found.side = found->side;
    notice.found.area_id = found->area_id;
    push_match_notice(notice);
}

void on_matchmaking_go(const RoverMatchmakingGo* go) {
    if (go == nullptr)
        return;
    MatchmakingNotice notice{};
    notice.kind = MatchmakingNoticeKind::go;
    copy_match_id(notice.go.offer_id, sizeof notice.go.offer_id, go->offer_id);
    notice.go.result = go->result;
    notice.go.role = go->role;
    notice.go.area_id = go->area_id;
    copy_match_id(notice.go.game_id, sizeof notice.go.game_id, go->game_id);
    notice.go.game_known = go->game_known;
    copy_match_id(notice.go.name, sizeof notice.go.name, go->name);
    notice.go.name_known = go->name_known;
    notice.go.game_type = go->game_type;
    notice.go.skill = go->skill;
    notice.go.seats = go->seats;
    notice.go.on_death = go->on_death;
    notice.go.side = go->side;
    notice.go.side_known = go->side_known;
    push_match_notice(notice);
}

RoverMatchmakingQuery wire_query(const MatchmakingQuery* query) {
    RoverMatchmakingQuery wire{};
    if (query == nullptr)
        return wire;
    wire.game_type = query->game_type;
    wire.skill_mode = query->skill_mode;
    wire.side = query->side;
    wire.planets = query->planets;
    wire.area_id = query->area_id;
    wire.area_known = query->area_known;
    return wire;
}

ServiceResult service_matchmaking_look(void*, const MatchmakingQuery* query) {
    if (query == nullptr)
        return ServiceResult::failed;
    const RoverMatchmakingQuery wire = wire_query(query);
    return accepted(rover_matchmaking_look(&wire));
}

ServiceResult service_matchmaking_join(void*, const MatchmakingQuery* query) {
    if (query == nullptr)
        return ServiceResult::failed;
    const RoverMatchmakingQuery wire = wire_query(query);
    return accepted(rover_matchmaking_join(&wire));
}

ServiceResult service_matchmaking_cancel(void*) {
    return accepted(rover_matchmaking_cancel());
}

ServiceResult service_matchmaking_answer(void*, const char* offer_id, int32_t accept) {
    return accepted(rover_matchmaking_answer(offer_id, accept));
}

bool service_matchmaking_take(void*, MatchmakingNotice* out) {
    if (out == nullptr || g_match_notice_count == 0)
        return false;
    *out = g_match_notices[0];
    if (g_match_notice_count > 1) {
        std::memmove(
            g_match_notices,
            g_match_notices + 1,
            sizeof(MatchmakingNotice) * static_cast<std::size_t>(g_match_notice_count - 1)
        );
    }
    --g_match_notice_count;
    return true;
}

void on_quickplay_status(const RoverQuickPlayStatus* status) {
    if (status == nullptr)
        return;
    QuickPlayNotice notice{};
    notice.kind = QuickPlayNoticeKind::status;
    notice.status.result = status->result;
    notice.status.queued = status->queued;
    notice.status.meter = status->meter;
    notice.status.fits = status->fits;
    notice.status.planets = status->planets;
    push_quick_notice(notice);
}

void on_quickplay_scan(const RoverQuickPlayScan* scan) {
    if (scan == nullptr)
        return;
    QuickPlayNotice notice{};
    notice.kind = QuickPlayNoticeKind::scan;
    notice.scan.seq = scan->seq;
    notice.scan.kind = scan->kind;
    notice.scan.reason = scan->reason;
    notice.scan.game_type = scan->game_type;
    notice.scan.skill = scan->skill;
    notice.scan.seats = scan->seats;
    notice.scan.seats_max = scan->seats_max;
    copy_match_id(notice.scan.planet, sizeof notice.scan.planet, scan->planet);
    copy_match_id(notice.scan.battle, sizeof notice.scan.battle, scan->battle);
    push_quick_notice(notice);
}

void on_quickplay_found(const RoverQuickPlayFound* found) {
    if (found == nullptr)
        return;
    QuickPlayNotice notice{};
    notice.kind = QuickPlayNoticeKind::found;
    copy_match_id(notice.found.offer_id, sizeof notice.found.offer_id, found->offer_id);
    notice.found.role = found->role;
    notice.found.area_id = found->area_id;
    copy_match_id(notice.found.game_id, sizeof notice.found.game_id, found->game_id);
    notice.found.game_known = found->game_known;
    copy_match_id(notice.found.name, sizeof notice.found.name, found->name);
    copy_match_id(notice.found.planet, sizeof notice.found.planet, found->planet);
    notice.found.game_type = found->game_type;
    notice.found.skill = found->skill;
    notice.found.seats = found->seats;
    notice.found.on_death = found->on_death;
    copy_match_id(notice.found.map, sizeof notice.found.map, found->map);
    notice.found.map_known = found->map_known;
    push_quick_notice(notice);
}

RoverQuickPlayQuery wire_quick_query(const QuickPlayQuery* query) {
    RoverQuickPlayQuery wire{};
    if (query == nullptr)
        return wire;
    wire.game_type = query->game_type;
    wire.where = query->where;
    wire.skill = query->skill;
    wire.side = query->side;
    wire.prefer_favorite = query->prefer_favorite;
    copy_match_id(wire.favorite, sizeof wire.favorite, query->favorite);
    wire.area_id = query->area_id;
    wire.area_known = query->area_known;
    return wire;
}

ServiceResult service_quickplay_join(void*, const QuickPlayQuery* query) {
    if (query == nullptr)
        return ServiceResult::failed;
    const RoverQuickPlayQuery wire = wire_quick_query(query);
    return accepted(rover_quickplay_join(&wire));
}

ServiceResult service_quickplay_cancel(void*) {
    return accepted(rover_quickplay_cancel());
}

ServiceResult service_quickplay_answer(void*, const char* offer_id, int32_t accept) {
    return accepted(rover_quickplay_answer(offer_id, accept));
}

bool service_quickplay_take(void*, QuickPlayNotice* out) {
    if (out == nullptr || g_quick_notice_count == 0)
        return false;
    *out = g_quick_notices[0];
    if (g_quick_notice_count > 1) {
        std::memmove(
            g_quick_notices,
            g_quick_notices + 1,
            sizeof(QuickPlayNotice) * static_cast<std::size_t>(g_quick_notice_count - 1)
        );
    }
    --g_quick_notice_count;
    return true;
}

} // namespace

RoverSession* rover_session() {
    return &g_session;
}

PeopleLists* rover_session_people_lists(RoverSession* session) {
    if (session->people_lists == nullptr) {
        auto* lists = static_cast<PeopleLists*>(std::malloc(sizeof(PeopleLists)));
        if (lists != nullptr)
            people_lists_init(lists);
        session->people_lists = lists;
    }
    return session->people_lists;
}

void rover_session_release_people_lists(RoverSession* session) {
    if (session->people_lists == nullptr)
        return;
    people_lists_release(session->people_lists);
    std::free(session->people_lists);
    session->people_lists = nullptr;
}

BoneyardsService rover_session_service() {
    BoneyardsService service{};
    service.user = &g_session;
    service.network_available = service_network_available;
    service.activate = service_activate;
    service.close = service_close;
    service.connected = service_connected;
    service.login = service_login;
    service.cancel_login = service_cancel_login;
    service.list_servers = service_list_servers;
    service.server_region = service_server_region;
    service.current_server = service_current_server;
    service.pump = service_pump;
    service.set_events = service_set_events;
    service.area_name = service_area_name;
    service.open_sector_map = service_open_sector_map;
    service.read_sector_map = service_read_sector_map;
    service.close_sector_map = service_close_sector_map;
    service.request_allegiance = service_request_allegiance;
    service.request_census = service_request_census;
    service.profile_templates = service_profile_templates;
    service.logout = service_logout;
    service.content_server = service_content_server;
    service.fetch_file = service_fetch_file;
    service.locate_user = service_locate_user;
    service.locate_game = service_locate_game;
    service.request_profile = service_request_profile;
    service.submit_profile = service_submit_profile;
    service.request_room_list = service_request_room_list;
    service.join_room = service_join_room;
    service.switch_room = service_switch_room;
    service.leave_room = service_leave_room;
    service.send_chat = service_send_chat;
    service.page_user = service_page_user;
    service.create_room = service_create_room;
    service.enter_battles = service_enter_battles;
    service.leave_battles = service_leave_battles;
    service.get_battle = service_get_battle;
    service.host_game = service_host_game;
    service.join_game = service_join_game;
    service.peoplelist_contains = service_peoplelist_contains;
    service.peoplelist_insert = service_peoplelist_insert;
    service.peoplelist_erase = service_peoplelist_erase;
    service.peoplelist_release = service_peoplelist_release;
    service.matchmaking_look = service_matchmaking_look;
    service.matchmaking_join = service_matchmaking_join;
    service.matchmaking_cancel = service_matchmaking_cancel;
    service.matchmaking_answer = service_matchmaking_answer;
    service.matchmaking_take = service_matchmaking_take;
    service.quickplay_join = service_quickplay_join;
    service.quickplay_cancel = service_quickplay_cancel;
    service.quickplay_answer = service_quickplay_answer;
    service.quickplay_take = service_quickplay_take;
    // A test that zeroes the session does not clear these queues. Installing the service does.
    rover_matchmaking_set_handlers(on_matchmaking_status, on_matchmaking_found, on_matchmaking_go);
    clear_match_notices();
    rover_quickplay_set_handlers(on_quickplay_status, on_quickplay_scan, on_quickplay_found);
    clear_quick_notices();
    boneyards_service_fill_offline(&service);
    return service;
}

void rover_session_idle(const RoverSession* session) {
    if (session != nullptr && session->pump_enabled != 0)
        rover_call_pump_events();
    // After the pump, so a reply it just took is stored before the session
    // says the dossier is loaded. No further pump and no sleep.
    if (!rover_session_settle_sector_fetch())
        return;
    RoverSession* s = rover_session();
    if (s->record.level == SessionLevel::logged_in) {
        s->record.level = SessionLevel::dossier_loaded;
        publish(s);
    }
}

bool rover_session_live(const RoverSession* session) {
    return session != nullptr && session->record.level >= SessionLevel::logged_in;
}

void rover_session_set_ad_event_note(AdEventNote note, void* context) {
    g_ad_event_note = note;
    g_ad_event_note_context = context;
}

void rover_session_report_ad_event(int32_t banner_id, int32_t kind, const char* lang) {
    char wire[kRoverRecordTextLimit + 1];
    wire[0] = '\0';
    if (!rover_send_ad_event(banner_id, kind, lang, wire, static_cast<int32_t>(sizeof wire)))
        return;
    if (g_ad_event_note != nullptr && wire[0] != '\0')
        g_ad_event_note(wire, g_ad_event_note_context);
}

const char* rover_session_selected_server() {
    const char* location = nullptr;
    rover_call_find_current_area(&location);
    if (location != nullptr)
        return location;
    return rover_session_uses_host_list() ? kHostServerLocation : "";
}

} // namespace oa::boneyards
