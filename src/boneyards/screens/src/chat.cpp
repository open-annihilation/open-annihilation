// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/chat.hpp"

#include "oa/boneyards/screens/player_pane.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace oa::boneyards::screens {
namespace {

constexpr const char* kTokenDelimiters = " \n\t";

void show_message(ChatConsole* c, const char* text) {
    if (c->ui.message_box != nullptr)
        c->ui.message_box(c->ui.user, text);
}

void set_activity(ChatConsole* c, bool running) {
    if (c->ui.activity != nullptr)
        c->ui.activity(c->ui.user, running);
}

void room_changed(ChatConsole* c, RowChange change, const ChatRoom* room, std::size_t index) {
    if (c->ui.room_row != nullptr)
        c->ui.room_row(c->ui.user, change, room, index);
}

void room_changed(ChatConsole* c, RowChange change, const ChatRoom* room) {
    room_changed(c, change, room, static_cast<std::size_t>(room - c->rooms));
}

void player_changed(ChatConsole* c, RowChange change, const ChatPlayer* player, std::size_t index) {
    if (c->ui.player_row != nullptr)
        c->ui.player_row(c->ui.user, change, player, index);
}

void player_changed(ChatConsole* c, RowChange change, const ChatPlayer* player) {
    player_changed(c, change, player, static_cast<std::size_t>(player - c->players));
}

void clear_rooms(ChatConsole* c) {
    c->room_total = 0;
    c->selected_room = -1;
    if (c->ui.rooms_cleared != nullptr)
        c->ui.rooms_cleared(c->ui.user);
}

void clear_players(ChatConsole* c) {
    c->player_total = 0;
    if (c->ui.players_cleared != nullptr)
        c->ui.players_cleared(c->ui.user);
}

void set_input(ChatConsole* c, const char* text) {
    copy_chars(c->input, sizeof c->input, text, kChatLineCharacters);
}

bool over_limit(const char* text, int32_t limit) {
    return utf8::characters(text) > limit;
}

// strtok-style scanner over the working line buffer.
struct Scanner {
    char* next;
};

char* scan_token(Scanner* s) {
    char* p = s->next;
    if (p == nullptr)
        return nullptr;
    while (*p != '\0' && std::strchr(kTokenDelimiters, *p) != nullptr)
        ++p;
    if (*p == '\0') {
        s->next = p;
        return nullptr;
    }
    char* token = p;
    while (*p != '\0' && std::strchr(kTokenDelimiters, *p) == nullptr)
        ++p;
    if (*p != '\0')
        *p++ = '\0';
    s->next = p;
    return token;
}

// Rejoins a trailing argument with the rest of the line by replacing the
// terminator the scanner wrote after it with a space.
void rejoin_rest(char* token, const char* line_end) {
    if (token == nullptr)
        return;
    char* end = token + std::strlen(token);
    if (end < line_end)
        *end = ' ';
}

bool is_one_of(const char* word, const char* a, const char* b, const char* c = nullptr) {
    return equals_nocase(word, a) || equals_nocase(word, b) ||
           (c != nullptr && equals_nocase(word, c));
}

void report_user_not_found(ChatConsole* c, const char* name) {
    char text[0x100];
    std::snprintf(text, sizeof text, "User '%s' not found in this chat room.", name);
    show_message(c, text);
}

bool guest_silenced_index(const ChatConsole* c, std::uint32_t id, std::size_t* index) {
    for (std::size_t i = 0; i < c->guest_silenced_count; ++i) {
        if (c->guest_silenced[i] == id) {
            *index = i;
            return true;
        }
    }
    return false;
}

void print_command_summary(ChatConsole* c) {
    static constexpr const char* kLines[] = {
        "Show this list: +? | +h | +help",
        "Whisper msg to user:  +w | +whisper <user> <msg>",
        "Lock whisper on user:  +wl | +whisperlock <user>",
        "Clear whisper lock:  -wl | -whisperlock",
        "Page msg to user:  +p | +page <user> <msg>",
        "Muzzle user:  +m | +muzzle <user>",
        "Stop muzzling user:  -m | -muzzle <user>",
        "Locate user:  +l | +locate <user>",
        "Create frequency:  +c | +create <frequency> [<description>]",
        "Create private frequency:  +cp | +createprivate <frequency> <password> [<description>]",
        "Go to frequency:  +g | +goto <frequency> [<password>]",
    };
    chat_print_line(c, "<<<< Command Summary >>>>>", kDefaultTextColor, false);
    for (const char* line : kLines)
        chat_print_line(c, line, kDefaultTextColor, true);
}

// Resolves the user argument shared by the whisper-lock and muzzle commands;
// returns 0 after reporting the problem.
std::uint32_t resolve_user_argument(ChatConsole* c, const char* name, const char* usage) {
    if (name == nullptr) {
        show_message(c, usage);
        return 0;
    }
    if (over_limit(name, kUserNameCharacters)) {
        show_message(c, "Usage:\n\nUser name too long");
        return 0;
    }
    const std::uint32_t id = chat_find_player_id_by_name(c, name);
    if (id == 0)
        report_user_not_found(c, name);
    return id;
}

void command_whisper(ChatConsole* c, Scanner* scan, const char* line_end) {
    char* name = scan_token(scan);
    char* message = scan_token(scan);
    if (name == nullptr || message == nullptr) {
        show_message(c, "Usage:\n\n+whisper username msg ...");
        return;
    }
    rejoin_rest(message, line_end);
    if (over_limit(name, kUserNameCharacters)) {
        show_message(c, "Usage:\n\nUser name too long");
        return;
    }
    const std::uint32_t target = chat_find_player_id_by_name(c, name);
    if (target == 0) {
        report_user_not_found(c, name);
        return;
    }
    char sent[0x200];
    std::snprintf(sent, sizeof sent, "<Whispered> %s", message);
    c->service->send_chat(c->service->user, c->session->room_id, c->session->area_id, target, sent);
    char echo[0x200];
    std::snprintf(echo, sizeof echo, "<You whispered to %s> %s", name, message);
    chat_print_line(c, echo, kDefaultTextColor, false);
    set_input(c, "");
}

void command_page(ChatConsole* c, Scanner* scan, const char* line_end) {
    char* name = scan_token(scan);
    char* message = scan_token(scan);
    if (name == nullptr || message == nullptr) {
        show_message(c, "Usage:\n\n+page username msg ...");
        return;
    }
    rejoin_rest(message, line_end);
    if (over_limit(name, kUserNameCharacters)) {
        show_message(c, "Usage:\n\nUser name too long");
        return;
    }
    if ((c->session->account_flags & kAccountUnregistered) != 0) {
        show_message(
            c,
            "Only registered users have access to this feature.\n"
            "Visit the Registration console to register."
        );
        return;
    }
    c->service->page_user(c->service->user, name, message);
    char echo[0x200];
    std::snprintf(echo, sizeof echo, "<You paged user %s> %s", name, message);
    chat_print_line(c, echo, kDefaultTextColor, false);
}

void command_create(ChatConsole* c, Scanner* scan, const char* line_end, bool with_password) {
    char* name = scan_token(scan);
    char* password = with_password ? scan_token(scan) : nullptr;
    char* description = scan_token(scan);
    if (name == nullptr || (with_password && password == nullptr)) {
        show_message(
            c,
            with_password ? "Usage:\n\n+createprivate <frequency> <password> [<description>] ..."
                          : "Usage:\n\n+create <frequency> [<description>] ..."
        );
        return;
    }
    rejoin_rest(description, line_end);
    if (over_limit(name, static_cast<int32_t>(kRoomNameMax))) {
        show_message(c, "Usage:\n\nRoom name too long");
        return;
    }
    if (with_password && over_limit(password, static_cast<int32_t>(kRoomPasswordMax))) {
        show_message(c, "Usage:\n\nRoom password too long");
        return;
    }
    if (description != nullptr &&
        over_limit(description, static_cast<int32_t>(kRoomDescriptionMax))) {
        show_message(c, "Usage:\n\nRoom description too long");
        return;
    }
    if (!chat_create_room(c, name, description, password))
        show_message(c, "Room creation failed");
}

void command_goto(ChatConsole* c, Scanner* scan) {
    char* name = scan_token(scan);
    char* password = scan_token(scan);
    if (name == nullptr) {
        show_message(c, "Usage:\n\n+goto room [pw] ...");
        return;
    }
    if (over_limit(name, static_cast<int32_t>(kRoomNameMax))) {
        show_message(c, "Usage:\n\nRoom name too long");
        return;
    }
    if (password != nullptr && over_limit(password, static_cast<int32_t>(kRoomPasswordMax))) {
        show_message(c, "Usage:\n\nRoom password too long");
        return;
    }
    char text[0x100];
    const ChatRoom* room = chat_find_room_by_name(c, name);
    if (room == nullptr) {
        std::snprintf(text, sizeof text, "Frequency '%s' not found in this chat area.", name);
        show_message(c, text);
        return;
    }
    if (room->password_required && password == nullptr) {
        std::snprintf(text, sizeof text, "Frequency '%s' requires password.", name);
        show_message(c, text);
        return;
    }
    if (c->session->room_id == room->room_id) {
        std::snprintf(text, sizeof text, "Already in frequency '%s'", name);
        show_message(c, text);
        return;
    }
    if (c->session->pending_room_id == room->room_id) {
        std::snprintf(text, sizeof text, "Already trying to go to frequency '%s'", name);
        show_message(c, text);
        return;
    }
    chat_go_to_room(c, c->session->area_id, room->room_id, password, name);
}

void command_whisper_lock(ChatConsole* c, Scanner* scan) {
    const std::uint32_t id =
        resolve_user_argument(c, scan_token(scan), "Usage:\n\n+whisperlock username ...");
    if (id != 0)
        chat_set_whisper_lock(c, id, true);
}

void command_muzzle(ChatConsole* c, Scanner* scan, bool muzzle) {
    const std::uint32_t id = resolve_user_argument(
        c,
        scan_token(scan),
        muzzle ? "Usage:\n\n+muzzle username ..." : "Usage:\n\n-muzzle username ..."
    );
    if (id != 0 && chat_is_silenced(c, id) != muzzle)
        chat_toggle_silence(c, id);
}

void command_locate(ChatConsole* c, Scanner* scan) {
    const char* name = scan_token(scan);
    if (name == nullptr) {
        show_message(c, "Usage:\n\n+locate username");
        return;
    }
    if (over_limit(name, kUserNameCharacters)) {
        show_message(c, "Usage:\n\nUser name too long");
        return;
    }
    c->service->locate_user(c->service->user, name, chat_on_locate_result, c);
}

void close_create_dialog(ChatConsole* c) {
    c->create_dialog.visible = false;
}

constexpr std::int32_t kEntryTakesInput = 1;

// The pane counts as shown only while the console holding it is.
bool create_dialog_shown(const ChatConsole* c) {
    return c->visible && c->create_dialog.visible;
}

/// Opens the hidden create-frequency pane, emptying its entries unless a request is pending.
///
/// @param[in,out] c chat console
void create_dialog_open(ChatConsole* c) {
    FrequencyDialog* d = &c->create_dialog;
    if (create_dialog_shown(c))
        return;
    if (!d->submitting) {
        d->name[0] = '\0';
        d->password[0] = '\0';
        d->description[0] = '\0';
    }
    d->visible = true;
}

/// Shows the whisper-lock icon of the player the pane holds as locked or not.
///
/// @param[in,out] pane player pane; one without a player and record is left alone
/// @param locked whether whispers are locked onto the player
void pane_set_whisper_icon(PlayerPane& pane, bool locked) {
    if (pane.user_id != 0 && pane.record != nullptr)
        pane.whisper_icon_value = locked ? widget_value::icon_on : widget_value::icon_off;
}

/// Shows the pane player's muzzle icon; it stays disabled for a SysOp.
///
/// @param[in,out] pane player pane; one without a player and record is left alone
/// @param silenced whether the player is muzzled
void pane_set_silence(PlayerPane& pane, bool silenced) {
    if (pane.user_id == 0 || pane.record == nullptr)
        return;
    if (pane.sysop)
        pane.silence_value = widget_value::disabled;
    else
        pane.silence_value = silenced ? widget_value::icon_on : widget_value::icon_off;
}

// Hides the rank icon and insignia, empties the name and rank, returns the
// current tab label to the normal colour and disables the pane's controls.
void blank_pane(PlayerPane* p) {
    p->user_icon_shown = false;
    p->name[0] = '\0';
    p->rank = "";
    p->insignia_shown = false;
    p->tab_highlighted[p->tab] = false;
    p->whisper_icon_value = widget_value::disabled;
    p->silence_value = widget_value::disabled;
    p->page_value = widget_value::disabled;
    for (int32_t& value : p->tab_values)
        value = widget_value::disabled;
}

void remember_area_room(ChatConsole* c, std::uint32_t area_id, std::uint32_t room_id) {
    for (std::size_t i = 0; i < c->area_room_count; ++i) {
        if (c->area_rooms[i].area_id == area_id) {
            c->area_rooms[i].room_id = room_id;
            return;
        }
    }
    if (c->area_room_count < kMaxAreaRooms)
        c->area_rooms[c->area_room_count++] = AreaRoom{area_id, room_id};
}

std::uint32_t preferred_area_room(const ChatConsole* c, std::uint32_t area_id) {
    for (std::size_t i = 0; i < c->area_room_count; ++i)
        if (c->area_rooms[i].area_id == area_id)
            return c->area_rooms[i].room_id;
    return 0;
}

// Where a new row of `type` named `name` goes: planetary defaults first in
// name order, user-created frequencies before the first battle debriefing,
// anything else last.
std::size_t room_insert_index(const ChatConsole* c, std::int32_t type, const char* name) {
    if (type == room_type::battle_debriefing)
        return c->room_total;
    for (std::size_t i = 0; i < c->room_total; ++i) {
        const ChatRoom& row = c->rooms[i];
        if (type == room_type::user_created) {
            if (row.type == room_type::battle_debriefing)
                return i;
        } else if (type == room_type::planetary_default) {
            if (row.type != room_type::planetary_default || compare_word_sort(name, row.name) <= 0)
                return i;
        }
    }
    return c->room_total;
}

// Colour of a chat line from its sender's User_Icons frame.
std::int32_t chat_color_for_icon(const ChatSession* s, std::int32_t icon) {
    if (icon == user_icon::sysop)
        return s->system_font;
    if (icon == user_icon::newsbot)
        return s->rank_0x15_font;
    if (icon == user_icon::core_high_commander || icon == user_icon::core_supreme_commander ||
        icon == user_icon::core_system_commander || icon == user_icon::core_repeat_high_commander ||
        icon == user_icon::core_repeat_supreme_commander)
        return s->core_font;
    if (icon == user_icon::arm_high_commander || icon == user_icon::arm_supreme_commander ||
        icon == user_icon::arm_system_commander || icon == user_icon::arm_repeat_high_commander ||
        icon == user_icon::arm_repeat_supreme_commander)
        return s->arm_font;
    return kDefaultTextColor;
}

} // namespace

void chat_create_dialog_build(ChatConsole* c) {
    FrequencyDialog* d = &c->create_dialog;
    d->visible = false;
    d->submitting = false;
    d->password[0] = '\0';
    d->name[0] = '\0';
    d->description[0] = '\0';
    d->entry_value = kEntryTakesInput;
    d->create_value = widget_value::disabled;
}

void chat_room_info_build(ChatConsole* c) {
    c->room_info_visible = false;
    c->room_info_room_id = kNoRoomInfo;
}

void chat_console_init(
    ChatConsole* c, ChatSession* session, const BoneyardsService* service, ChatUi ui
) {
    std::memset(c, 0, sizeof *c);
    c->session = session;
    c->service = service;
    c->ui = ui;
    c->selected_room = -1;
    chat_pane_build(c);
    chat_create_dialog_build(c);
    chat_room_info_build(c);
}

void chat_print_line(ChatConsole* c, const char* text, std::int32_t color, bool quiet) {
    if (c->ui.print_line != nullptr)
        c->ui.print_line(c->ui.user, text, color, quiet);
}

ChatPlayer* chat_find_player(ChatConsole* c, std::uint32_t user_id) {
    for (std::size_t i = 0; i < c->player_total; ++i)
        if (c->players[i].user_id == user_id)
            return &c->players[i];
    return nullptr;
}

std::uint32_t chat_find_player_id_by_name(const ChatConsole* c, const char* name) {
    for (std::size_t i = 0; i < c->player_total; ++i)
        if (equals_nocase(c->players[i].name, name))
            return c->players[i].user_id;
    return 0;
}

ChatRoom* chat_find_room(ChatConsole* c, std::uint32_t room_id) {
    for (std::size_t i = 0; i < c->room_total; ++i)
        if (c->rooms[i].room_id == room_id)
            return &c->rooms[i];
    return nullptr;
}

ChatRoom* chat_find_room_by_name(ChatConsole* c, const char* name) {
    for (std::size_t i = 0; i < c->room_total; ++i)
        if (equals_nocase(c->rooms[i].name, name))
            return &c->rooms[i];
    return nullptr;
}

void chat_add_room_population(ChatConsole* c, std::uint32_t room_id, std::int32_t delta) {
    if (ChatRoom* room = chat_find_room(c, room_id)) {
        room->population += delta;
        room_changed(c, RowChange::updated, room);
    }
}

bool chat_format_player_name(
    const ChatConsole* c, std::uint32_t user_id, char* out, std::size_t capacity
) {
    for (std::size_t i = 0; i < c->player_total; ++i) {
        if (c->players[i].user_id == user_id) {
            utf8::copy(out, capacity, c->players[i].name, 0);
            return true;
        }
    }
    std::snprintf(out, capacity, "UNK");
    return false;
}

bool chat_is_silenced(const ChatConsole* c, std::uint32_t user_id) {
    if ((user_id & kGuestUserBit) == 0)
        return c->service->peoplelist_contains(c->service->user, kSilencedList, user_id);
    std::size_t index = 0;
    return guest_silenced_index(c, user_id, &index);
}

bool chat_toggle_silence(ChatConsole* c, std::uint32_t user_id) {
    bool silenced = false;
    if ((user_id & kGuestUserBit) == 0) {
        if (c->service->peoplelist_contains(c->service->user, kSilencedList, user_id)) {
            c->service->peoplelist_erase(c->service->user, kSilencedList, user_id);
        } else {
            c->service->peoplelist_insert(c->service->user, kSilencedList, user_id);
            silenced = true;
        }
    } else {
        std::size_t index = 0;
        if (guest_silenced_index(c, user_id, &index)) {
            c->guest_silenced[index] = c->guest_silenced[--c->guest_silenced_count];
        } else if (c->guest_silenced_count < kMaxGuestSilenced) {
            c->guest_silenced[c->guest_silenced_count++] = user_id;
            silenced = true;
        }
    }
    if (c->pane.visible && user_id == c->pane.user_id) {
        pane_set_silence(c->pane, silenced);
        chat_pane_changed(c, false);
    }
    if (ChatPlayer* row = chat_find_player(c, user_id)) {
        row->silenced = silenced;
        player_changed(c, RowChange::updated, row);
    }
    return silenced;
}

void chat_set_whisper_lock(ChatConsole* c, std::uint32_t user_id, bool lock) {
    if (!lock) {
        c->whisper_lock_user = 0;
        chat_print_line(c, "[Whisper Lock Deactivated]", c->session->system_font, false);
        ChatPlayer* row = chat_find_player(c, user_id);
        if (row != nullptr) {
            row->whisper_locked = false;
            player_changed(c, RowChange::updated, row);
        }
        if (c->pane.visible && user_id == c->pane.user_id) {
            pane_set_whisper_icon(c->pane, false);
            if (row == nullptr)
                chat_pane_disable_whisper_icon(&c->pane);
            chat_pane_changed(c, false);
        }
        return;
    }
    if (c->whisper_lock_user == user_id)
        return;
    char name[0x20];
    if (!chat_format_player_name(c, user_id, name, sizeof name))
        return;
    if (c->whisper_lock_user != 0) {
        if (ChatPlayer* old = chat_find_player(c, c->whisper_lock_user)) {
            old->whisper_locked = false;
            player_changed(c, RowChange::updated, old);
        }
        if (c->pane.visible && c->whisper_lock_user == c->pane.user_id) {
            pane_set_whisper_icon(c->pane, false);
            chat_pane_changed(c, false);
        }
    }
    if (ChatPlayer* row = chat_find_player(c, user_id)) {
        row->whisper_locked = true;
        player_changed(c, RowChange::updated, row);
    }
    c->whisper_lock_user = user_id;
    char text[0x80];
    std::snprintf(text, sizeof text, "[Whisper Lock on '%s' Activated]", name);
    chat_print_line(c, text, c->session->system_font, false);
    if (c->pane.visible && user_id == c->pane.user_id) {
        pane_set_whisper_icon(c->pane, true);
        chat_pane_changed(c, false);
    }
}

bool chat_submit_input(ChatConsole* c) {
    char line[kChatLineCapacity];
    copy_chars(line, sizeof line, c->input, kChatLineCharacters);
    set_input(c, "");

    char* cursor = line;
    while (*cursor == ' ')
        ++cursor;
    if (*cursor == '\0')
        return false;

    if (*cursor != '+' && *cursor != '-') {
        if (c->whisper_lock_user == 0) {
            c->service->send_chat(
                c->service->user, c->session->room_id, c->session->area_id, 0, line
            );
            return true;
        }
        char name[0x20];
        if (!chat_format_player_name(c, c->whisper_lock_user, name, sizeof name)) {
            chat_print_line(
                c, "[Whisper failed - Message Not Sent]", c->session->system_font, false
            );
            chat_set_whisper_lock(c, c->whisper_lock_user, false);
            set_input(c, line);
            return true;
        }
        // Rewrite the line as "+w <name> <text>" within the line buffer.
        char rewritten[kChatLineCapacity];
        utf8::copy(rewritten, sizeof rewritten, "+w ", 0);
        std::size_t used = std::strlen(rewritten);
        utf8::copy(rewritten + used, sizeof rewritten - used, name, 0);
        used = std::strlen(rewritten);
        if (used + 1 < sizeof rewritten) {
            rewritten[used] = ' ';
            rewritten[used + 1] = '\0';
            ++used;
        }
        utf8::copy(rewritten + used, sizeof rewritten - used, line, 0);
        utf8::copy(line, sizeof line, rewritten, kChatLineCharacters);
        cursor = line;
    }

    char whole_line[kChatLineCapacity];
    copy_bounded(whole_line, sizeof whole_line, cursor);
    const char* line_end = cursor + std::strlen(cursor);
    Scanner scan{cursor};
    const char* word = scan_token(&scan);
    if (word == nullptr)
        word = "";

    if (is_one_of(word, "+?", "+h", "+help"))
        print_command_summary(c);
    else if (is_one_of(word, "+w", "+whisper"))
        command_whisper(c, &scan, line_end);
    else if (is_one_of(word, "+wl", "+whisperlock"))
        command_whisper_lock(c, &scan);
    else if (is_one_of(word, "-wl", "-whisperlock")) {
        if (c->whisper_lock_user != 0)
            chat_set_whisper_lock(c, c->whisper_lock_user, false);
    } else if (is_one_of(word, "+p", "+page"))
        command_page(c, &scan, line_end);
    else if (is_one_of(word, "+m", "+muzzle"))
        command_muzzle(c, &scan, true);
    else if (is_one_of(word, "-m", "-muzzle"))
        command_muzzle(c, &scan, false);
    else if (is_one_of(word, "+l", "+locate"))
        command_locate(c, &scan);
    else if (is_one_of(word, "+c", "+create"))
        command_create(c, &scan, line_end, false);
    else if (is_one_of(word, "+cp", "+createprivate"))
        command_create(c, &scan, line_end, true);
    else if (is_one_of(word, "+g", "+goto"))
        command_goto(c, &scan);
    else
        c->service->send_chat(
            c->service->user, c->session->room_id, c->session->area_id, 0, whole_line
        );
    return true;
}

bool chat_dispatch_message(ChatConsole* c, std::int32_t code) {
    if (code == chat_message::enter_key)
        return c->input_focused ? chat_submit_input(c) : true;
    const bool scroll_code = code == 400 || code == 401 || code == 403 || code == 405;
    return scroll_code;
}

void chat_receive_message(
    ChatConsole* c, std::uint32_t sender_id, const char* text, bool /*from_server*/
) {
    // A line from the server names no member of the frequency. A speaker
    // with no row is not shown, so that line is not shown either.
    ChatPlayer* sender = chat_find_player(c, sender_id);
    if (sender == nullptr)
        return;
    if (sender->user_icon != user_icon::sysop && sender_id != c->session->self_user_id &&
        chat_is_silenced(c, sender_id))
        return;
    char name[0x20];
    chat_format_player_name(c, sender_id, name, sizeof name);
    char line[0x1c0];
    std::snprintf(
        line,
        sizeof line,
        "%s%s",
        name,
        sender->user_icon == user_icon::sysop ? " [SysOp] : " : ": "
    );
    const std::size_t used = std::strlen(line);
    const std::size_t budget = std::min(kLobbyMessageMax, sizeof line - 1 - used);
    utf8::copy(line + used, budget + 1, text, 0);
    chat_print_line(c, line, chat_color_for_icon(c->session, sender->user_icon), false);
}

void chat_update_player(
    ChatConsole* c,
    PlayerListChange change,
    std::uint32_t user_id,
    const char* name,
    std::int32_t latency_icon,
    std::int32_t icon,
    std::int32_t rank,
    const char* allegiance
) {
    if (change == PlayerListChange::joined) {
        if (user_id != 0 && c->pane.visible && user_id == c->pane.user_id) {
            chat_pane_whisper_icon_off(&c->pane);
            chat_pane_changed(c, false);
        }
        if (icon <= 0 || icon >= user_icon::limit)
            return;
        ChatPlayer* row = chat_find_player(c, user_id);
        const bool added = row == nullptr;
        if (added) {
            ++c->player_count;
            if (c->player_total >= kMaxChatPlayers)
                return;
            row = &c->players[c->player_total++];
            std::memset(row, 0, sizeof *row);
            copy_bounded(row->name, sizeof row->name, name);
            row->user_id = user_id;
            row->silenced = chat_is_silenced(c, user_id);
            row->whisper_locked = c->whisper_lock_user == user_id;
            chat_add_room_population(c, c->session->room_id, 1);
        }
        row->latency_icon = latency_icon;
        row->user_icon = icon;
        row->rank = rank;
        copy_bounded(row->allegiance, sizeof row->allegiance, allegiance);
        player_changed(c, added ? RowChange::added : RowChange::updated, row);
        return;
    }
    if (user_id != 0) {
        if (user_id == c->whisper_lock_user)
            chat_set_whisper_lock(c, user_id, false);
        if (c->pane.visible && user_id == c->pane.user_id) {
            chat_pane_disable_whisper_icon(&c->pane);
            chat_pane_changed(c, false);
        }
    }
    for (std::size_t i = 0; i < c->player_total; ++i) {
        if (c->players[i].user_id != user_id)
            continue;
        const ChatPlayer gone = c->players[i];
        for (std::size_t j = i + 1; j < c->player_total; ++j)
            c->players[j - 1] = c->players[j];
        --c->player_total;
        player_changed(c, RowChange::removed, &gone, i);
        chat_add_room_population(c, c->session->room_id, -1);
        --c->player_count;
        return;
    }
}

void chat_format_locate_result(const LocateReport* r, char* out, std::size_t capacity) {
    const char* who = r->user_name;
    switch (r->status) {
    case static_cast<std::int32_t>(LocateStatus::logged_out):
        std::snprintf(out, capacity, "%s is not currently logged in.", who);
        break;
    case static_cast<std::int32_t>(LocateStatus::chatting):
        std::snprintf(
            out,
            capacity,
            "%s is currently chatting on Planet %s Frequency %s.",
            who,
            r->area_name,
            r->room_name
        );
        break;
    case static_cast<std::int32_t>(LocateStatus::examining_battles):
        std::snprintf(
            out, capacity, "%s is currently examining battles on Planet %s.", who, r->area_name
        );
        break;
    case static_cast<std::int32_t>(LocateStatus::in_battle):
        std::snprintf(
            out,
            capacity,
            "%s is currently engaged in Battle %s on Planet %s.",
            who,
            r->game_name,
            r->area_name
        );
        break;
    case static_cast<std::int32_t>(LocateStatus::other_server):
        std::snprintf(out, capacity, "%s is currently on Server %s.", who, r->server_name);
        break;
    case static_cast<std::int32_t>(LocateStatus::no_such_user):
        std::snprintf(out, capacity, "%s does not exist.  Check spelling and try again.", who);
        break;
    case static_cast<std::int32_t>(LocateStatus::viewing_pages):
        std::snprintf(
            out, capacity, "%s is currently viewing the Map, News, Help, Ladders, or Recon.", who
        );
        break;
    default:
        std::snprintf(
            out, capacity, "There has been a locator system error.  Please try again later."
        );
        break;
    }
}

void chat_on_locate_result(void* context, const LocateReport* report) {
    auto* c = static_cast<ChatConsole*>(context);
    char line[0x100];
    chat_format_locate_result(report, line, sizeof line);
    chat_print_line(c, line, kDefaultTextColor, false);
}

void chat_on_join_result(ChatConsole* c, JoinResult result) {
    ChatSession* s = c->session;
    char text[0x100];
    bool close_room_info = false;
    if (result == JoinResult::entered) {
        std::snprintf(text, sizeof text, "[Entered '%s']", s->pending_room_name);
        c->enter_default_room = false;
        s->room_id = s->pending_room_id;
        c->player_count = 0;
        clear_players(c);
        remember_area_room(c, s->area_id, s->room_id);
        if (c->create_dialog.visible)
            close_create_dialog(c);
        close_room_info = c->room_info_visible;
    } else if (s->room_id == 0) {
        std::snprintf(
            text, sizeof text, "[Entry to '%s' failed - Attempting a Default]", s->pending_room_name
        );
        c->enter_default_room = true;
    } else if (
        c->room_info_visible && c->room_info_room_id == s->pending_room_id &&
        chat_find_room(c, c->room_info_room_id) == nullptr
    ) {
        std::snprintf(
            text,
            sizeof text,
            "[Entry to '%s' failed - The frequency no longer exists]",
            s->pending_room_name
        );
        close_room_info = true;
    } else {
        std::snprintf(text, sizeof text, "[Entry to '%s' failed]", s->pending_room_name);
    }
    if (close_room_info)
        c->room_info_visible = false;
    s->pending_area_id = 0;
    s->pending_room_id = 0;
    s->pending_room_name[0] = '\0';
    chat_print_line(c, text, s->system_font, false);
    clear_rooms(c);
    c->service->request_room_list(c->service->user, s->area_id);
}

void chat_on_room_listed(
    ChatConsole* c, PlayerListChange change, const RoomListing* listing, bool last
) {
    if (c->room_total == 0)
        c->default_candidate_count = 0;
    chat_update_room_row(c, change, listing);
    if (last) {
        set_activity(c, false);
        const ChatRoom* current = chat_find_room(c, c->session->room_id);
        if (current != nullptr && c->ui.show_room != nullptr)
            c->ui.show_room(c->ui.user, static_cast<std::size_t>(current - c->rooms));
    }
    if (change != PlayerListChange::joined || !c->enter_default_room)
        return;
    if (listing->type == room_type::planetary_default &&
        listing->population <= kDefaultRoomPopulationMax &&
        c->default_candidate_count < kMaxChatRooms)
        c->default_candidates[c->default_candidate_count++] = listing->room_id;
    if (!last)
        return;
    if (c->default_candidate_count == 0) {
        show_message(c, "Unable to enter chat on this planet. Please try another.");
        return;
    }
    const std::uint32_t roll = c->ui.random != nullptr ? c->ui.random(c->ui.user) : 0;
    const std::uint32_t room_id =
        c->default_candidates[roll % static_cast<std::uint32_t>(c->default_candidate_count)];
    const ChatRoom* room = chat_find_room(c, room_id);
    c->enter_default_room = false;
    set_activity(c, true);
    c->service->join_room(
        c->service->user, c->session->area_id, room_id, nullptr, room != nullptr ? room->name : ""
    );
    c->session->pending_room_id = room_id;
    c->session->pending_area_id = c->session->area_id;
    copy_chars(
        c->session->pending_room_name,
        sizeof c->session->pending_room_name,
        room != nullptr ? room->name : "",
        static_cast<int32_t>(kRoomNameMax)
    );
}

namespace {

/// Builds a frequency row for a listed type and inserts it in the list's order.
///
/// @param[in,out] c chat console
/// @param listing the row's fields
/// @return the row, or null for an unlisted type or a full list
ChatRoom* add_room_row(ChatConsole* c, const RoomListing* listing) {
    if (listing->type == room_type::unlisted || c->room_total >= kMaxChatRooms)
        return nullptr;
    ChatRoom added{};
    added.room_id = listing->room_id;
    added.password_required = listing->password_required;
    added.type = listing->type;
    copy_chars(added.name, sizeof added.name, listing->name, static_cast<int32_t>(kRoomNameMax));
    copy_chars(
        added.description,
        sizeof added.description,
        listing->description,
        static_cast<int32_t>(kRoomDescriptionMax)
    );
    copy_bounded(added.tag, sizeof added.tag, listing->tag);
    const std::size_t index = room_insert_index(c, added.type, added.name);
    for (std::size_t j = c->room_total; j > index; --j)
        c->rooms[j] = c->rooms[j - 1];
    ++c->room_total;
    if (c->selected_room >= static_cast<std::int32_t>(index))
        ++c->selected_room;
    c->rooms[index] = added;
    return &c->rooms[index];
}

} // namespace

void chat_update_room_row(ChatConsole* c, PlayerListChange change, const RoomListing* listing) {
    ChatRoom* row = chat_find_room(c, listing->room_id);
    if (change == PlayerListChange::joined) {
        const bool added = row == nullptr;
        if (added)
            row = add_room_row(c, listing);
        if (row == nullptr)
            return;
        row->population =
            c->session->room_id == listing->room_id ? c->player_count : listing->population;
        room_changed(c, added ? RowChange::added : RowChange::updated, row);
        return;
    }
    if (change != PlayerListChange::left || row == nullptr)
        return;
    const ChatRoom gone = *row;
    const auto index = static_cast<std::int32_t>(row - c->rooms);
    for (std::size_t j = static_cast<std::size_t>(index) + 1; j < c->room_total; ++j)
        c->rooms[j - 1] = c->rooms[j];
    --c->room_total;
    if (c->selected_room == index)
        c->selected_room = -1;
    else if (c->selected_room > index)
        --c->selected_room;
    room_changed(c, RowChange::removed, &gone, static_cast<std::size_t>(index));
}

bool chat_go_to_room(
    ChatConsole* c,
    std::uint32_t area_id,
    std::uint32_t room_id,
    const char* password,
    const char* name
) {
    ChatSession* s = c->session;
    if (s->pending_room_id != 0 || (s->area_id == area_id && s->room_id == room_id))
        return false;
    set_activity(c, true);
    c->service->switch_room(
        c->service->user, area_id, room_id, password, s->area_id, s->room_id, name
    );
    s->pending_area_id = area_id;
    s->pending_room_id = room_id;
    copy_chars(
        s->pending_room_name,
        sizeof s->pending_room_name,
        name != nullptr ? name : "",
        static_cast<int32_t>(kRoomNameMax)
    );
    return true;
}

void chat_open_on_planet(ChatConsole* c) {
    ChatSession* s = c->session;
    c->selected_room = -1;
    set_activity(c, true);
    s->room_id = 0;
    const char* label = nullptr;
    if (!s->debriefing_pending) {
        const std::uint32_t preferred = preferred_area_room(c, s->area_id);
        if (preferred == 0) {
            remember_area_room(c, s->area_id, 0);
            c->enter_default_room = true;
            c->service->request_room_list(c->service->user, s->area_id);
        } else {
            c->enter_default_room = false;
            c->service->join_room(
                c->service->user, s->area_id, preferred, nullptr, "Preferred Room"
            );
            s->pending_area_id = s->area_id;
            s->pending_room_id = preferred;
            label = "Preferred Room";
        }
    } else {
        c->enter_default_room = false;
        c->service->join_room(
            c->service->user, s->debrief_area_id, s->debrief_room_id, nullptr, "Debriefing Room"
        );
        s->pending_area_id = s->debrief_area_id;
        s->pending_room_id = s->debrief_room_id;
        label = "Debriefing Room";
    }
    if (label != nullptr)
        copy_chars(
            s->pending_room_name,
            sizeof s->pending_room_name,
            label,
            static_cast<int32_t>(kRoomNameMax)
        );
    c->input_focused = true;
    c->visible = true;
}

void chat_handle_pane_message(
    ChatConsole* c, std::int32_t code, std::uint32_t user_id, const char* user_name
) {
    switch (code) {
    case chat_message::clear_selection:
        if (c->ui.select_player != nullptr)
            c->ui.select_player(c->ui.user, -1);
        break;
    case chat_message::go_to_room:
        if (user_name != nullptr && user_name[0] != '\0') {
            char text[0x40];
            utf8::copy(text, sizeof text, "+Page ", 0);
            const std::size_t used = std::strlen(text);
            utf8::copy(text + used, sizeof text - used, user_name, 0);
            const std::size_t end = std::strlen(text);
            if (end + 1 < sizeof text) {
                text[end] = ' ';
                text[end + 1] = '\0';
            }
            set_input(c, text);
            c->input_focused = true;
            if (c->ui.show_input != nullptr)
                c->ui.show_input(c->ui.user);
        }
        break;
    case chat_message::toggle_muzzle:
        chat_toggle_silence(c, user_id);
        break;
    case chat_message::lock_whisper:
        if (user_id != 0)
            chat_set_whisper_lock(c, user_id, user_id != c->whisper_lock_user);
        break;
    case chat_message::profile_shown:
        if (user_id == c->whisper_lock_user)
            pane_set_whisper_icon(c->pane, true);
        if (user_id != c->session->self_user_id)
            pane_set_silence(c->pane, chat_is_silenced(c, user_id));
        chat_pane_changed(c, false);
        break;
    case chat_message::unlock_whisper:
        if (user_id != 0 && user_id == c->whisper_lock_user)
            chat_set_whisper_lock(c, user_id, false);
        break;
    default:
        break;
    }
}

void chat_on_control(ChatConsole* c, ChatControl control, std::int32_t code, std::size_t index) {
    ChatSession* s = c->session;
    switch (control) {
    case ChatControl::room_list: {
        if (index >= c->room_total)
            return;
        c->selected_room = static_cast<std::int32_t>(index);
        const ChatRoom* room = &c->rooms[index];
        if (code == kListActivate && !room->password_required && room->room_id != s->room_id &&
            s->pending_room_id == 0) {
            chat_go_to_room(c, s->area_id, room->room_id, nullptr, room->name);
        } else {
            if (!c->room_info_visible) {
                c->pane.visible = false;
                c->create_dialog.visible = false;
                c->room_info_visible = true;
            }
            c->room_info_room_id = room->room_id;
        }
        break;
    }
    case ChatControl::create_button:
        if (!c->create_dialog.visible) {
            chat_close_panes(c);
            create_dialog_open(c);
        } else {
            c->create_dialog.visible = false;
        }
        break;
    case ChatControl::player_list:
        if (index >= c->player_total)
            return;
        if (!c->pane.visible) {
            c->create_dialog.visible = false;
            c->room_info_visible = false;
            c->pane.visible = true;
        }
        chat_pane_begin_profile(
            c, c->players[index].user_id, c->players[index].user_icon == user_icon::sysop
        );
        break;
    case ChatControl::matchmaking_button:
        show_message(c, "Matchmaking coming soon!");
        break;
    }
}

void chat_pane_reset(ChatConsole* c) {
    if (c->pane.visible)
        return;
    blank_pane(&c->pane);
    c->pane.html = kPaneEmptyPage;
    chat_pane_changed(c, true);
}

void chat_pane_begin_profile(ChatConsole* c, std::uint32_t user_id, bool sysop) {
    chat_pane_start_activity_light(c);
    blank_pane(&c->pane);
    c->pane.html = "<html><BODY><b><font size=+2>ACCESSING...</b></font></BODY></html>";
    // Building the pane's record reads Profile.def the first time.
    (void)profile_read_templates(c->service);
    profile_record_release(&c->pane.profile);
    c->pane.record = &c->pane.profile;
    c->pane.user_id = user_id;
    c->pane.sysop = sysop;
    c->service->request_profile(
        c->service->user, user_id, chat_pane_on_profile, chat_pane_on_ladder, c
    );
    chat_pane_changed(c, true);
}

void chat_create_dialog_pulse(ChatConsole* c) {
    FrequencyDialog* d = &c->create_dialog;
    if (!create_dialog_shown(c))
        return;
    // The frequency change is the session's pending room; the name
    // test reads the entry's text length.
    const bool idle = !d->submitting && c->session->pending_room_id == 0;
    if (!idle || d->name[0] == '\0')
        d->create_value = widget_value::disabled;
    else if (d->create_value == widget_value::disabled)
        d->create_value = widget_value::enabled; // any other face is kept
    d->entry_value = idle ? kEntryTakesInput : widget_value::disabled;
}

bool chat_create_room(
    ChatConsole* c, const char* name, const char* description, const char* password
) {
    FrequencyDialog* d = &c->create_dialog;
    if (d->submitting || c->session->pending_room_id != 0)
        return false;
    create_dialog_open(c);
    copy_chars(
        d->name, sizeof d->name, name != nullptr ? name : "", static_cast<int32_t>(kRoomNameMax)
    );
    copy_chars(
        d->password,
        sizeof d->password,
        password != nullptr ? password : "",
        static_cast<int32_t>(kRoomPasswordMax)
    );
    copy_chars(
        d->description,
        sizeof d->description,
        description != nullptr ? description : "",
        static_cast<int32_t>(kRoomDescriptionMax)
    );
    d->submitting = true;
    c->service->create_room(
        c->service->user, c->session->area_id, d->name, d->description, d->password
    );
    chat_create_dialog_pulse(c);
    return true;
}

void chat_on_room_created(
    ChatConsole* c, std::uint32_t area_id, std::uint32_t room_id, const char* name
) {
    c->create_dialog.submitting = false;
    if (area_id == 0 || room_id == 0) {
        show_message(c, "Unable to Create New Frequency");
        return;
    }
    chat_go_to_room(c, area_id, room_id, c->create_dialog.password, name);
}

void chat_reset_room_counts(ChatConsole* c) {
    c->session->room_id = 0;
    c->player_count = 0;
}

void chat_close_panes(ChatConsole* c) {
    if (c->pane.visible)
        chat_pane_close(&c->pane);
    c->create_dialog.visible = false;
    c->room_info_visible = false;
}

void chat_close(ChatConsole* c) {
    clear_players(c);
    clear_rooms(c);
    c->whisper_lock_user = 0;
    chat_close_panes(c);
    c->visible = false;
    if (c->session->room_id != 0)
        c->service->leave_room(c->service->user, c->session->area_id, c->session->room_id);
    c->service->peoplelist_release(c->service->user);
}

} // namespace oa::boneyards::screens
