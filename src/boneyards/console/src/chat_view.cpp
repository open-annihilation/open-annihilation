// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Chat Frequencies console: the planet's frequencies with CREATE and the
// players of the entered one in the left column, the ChatList log on the
// right and the input line under it. The chat itself is the navigation's
// screens::ChatConsole; this is its window, which prints the chat's lines
// and hands it the typed ones.
#include "oa/boneyards/utf8.hpp"

#include <cstring>

#include "console_state.hpp"
#include "oa/boneyards/console/feature_consoles.hpp"
#include "oa/boneyards/release.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/surface.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

constexpr const char* k_console_name = "ChatConsole";
constexpr const char* k_log_name = "ChatList";
constexpr const char* k_minimize_sequence = "Minimize_Button";
constexpr const char* k_green_button = "GREEN_BUTTON";
constexpr int32_t k_frame_state = 1;

namespace layout {
// Separators in the console's own image: down the left column's right edge,
// along the top of the input line and above PLAYERS (and MATCHMAKING).
constexpr uint8_t rule_color = 0x5d;
constexpr int32_t column_edge = 0x7b;
constexpr int32_t column_top = 3;
constexpr int32_t input_rule_y = 0x124;
constexpr int32_t players_rule_y = 0x62;

constexpr int32_t log_x = 0x7c, log_y = 2, log_width = 0x176, log_height = 0x122;
constexpr int32_t log_capacity = 100;
// The ChatList under an open pane.
constexpr int32_t log_pane_y = 0x9b, log_pane_height = 0x89;
constexpr int32_t prompt_x = 0x81, prompt_y = 0x131, prompt_width = 0x40;
constexpr int32_t input_x = 0xc2, input_y = 0x127, input_height = 0xe;
constexpr int32_t input_max = 0xf3;
constexpr uint8_t selection_r = 0x51, selection_g = 0, selection_b = 0, selection_a = 0xff;
constexpr int32_t title_y = 6, title_width = 0x7c;
constexpr int32_t rooms_y = 0x15, rooms_width = 0x7b, rooms_height = 0x35;
constexpr int32_t create_x = 10, create_y = 0x4a;
constexpr int32_t players_title_y = 0x66, players_title_width = 0x7b;
constexpr int32_t players_y = 0x76, players_width = 0x7b;
constexpr int32_t players_height = 0xbe, players_height_matchmaking = 0xad;
constexpr int32_t matchmaking_y = 0x127, matchmaking_width = 0x6c;
constexpr int32_t minimize_x = 0x6c, minimize_width = 0xd, minimize_height = 0xc;
// Continuation rows of a wrapped log line start this far in.
constexpr int32_t line_indent = 8;
// The first modern row starts this far in. A continuation uses line_indent.
constexpr int32_t modern_indent = 6;
} // namespace layout

// Label placements (lobby_label_create): centred in the given width, or
// left-aligned with the given point as the bottom-left corner.
constexpr int32_t k_align_centre = 4;
constexpr int32_t k_align_bottom_left = 6;
constexpr int32_t k_anchor_top_left = 0;
constexpr int32_t k_anchor_bottom_left = 2;

ChatView* as_chat(Window* w) {
    return reinterpret_cast<ChatView*>(w);
}

ConsoleNavButton* chat_button_of(const ChatView* view) {
    return reinterpret_cast<ConsoleNavButton*>(view->chat_button);
}

screens::ChatConsole* chat_of(const ChatView* view) {
    return &view->owner->navigation->chat;
}

void play_sound(const ChatView* view, const char* name) {
    const MainMenuHost& host = view->owner->config.host;
    if (host.play_sound != nullptr)
        host.play_sound(host.user, name);
}

// The console the chat prints to: set as it is made, cleared as it goes.
ChatView* g_open_chat = nullptr;

struct ModernPrint {
    ChatView* view;
    Handle font;
};

// One wrapped row: a text row at the modern height, its label in the line's colour.
void add_modern_row(void* user, const char* line, int32_t indent, int32_t height, uint8_t ink) {
    auto* print = static_cast<ModernPrint*>(user);
    const screens::HatFonts* fonts = &print->view->owner->system.hat_fonts;
    screens::ByTextRow* row =
        screens::by_text_row_create(print->view->log, line, fonts, false, indent);
    screens::by_text_row_set_font(row, print->font);
    window_set_size(&row->base, row->base.width, height);
    if (row->label != nullptr) {
        window_set_size(row->label, row->label->width, height);
        modern_chat_mark(row->label, ink);
    }
}

/// Wraps a chat line into the open console's ChatList, continuation rows eight pixels in.
///
/// Plays GREEN_BUTTON first unless the line is quiet. The default colour is
/// the list's small font. With modern fonts on, the rows use those faces at
/// the player's Text size and the first row starts six pixels in. The log
/// follows its end only when it was showing it.
///
/// @param user unused context of the chat's output hook
/// @param text the line
/// @param font Hat font of the line; screens::kDefaultTextColor for the small font
/// @param quiet whether to skip the sound
void chat_print_line(void* /*user*/, const char* text, int32_t font, bool quiet) {
    if (g_open_chat == nullptr)
        return;
    if (text != nullptr)
        boneyards_console_driver_note("said", text);
    if (!quiet)
        play_sound(g_open_chat, k_green_button);
    const screens::HatFonts& fonts = g_open_chat->owner->system.hat_fonts;
    const Handle face = font == screens::kDefaultTextColor ? fonts.small : font;
    ModernPrint print{g_open_chat, face};
    if (modern_chat_write(
            &g_open_chat->log->list,
            text,
            screens::by_list_row_width(&g_open_chat->log->list.base),
            layout::modern_indent,
            layout::line_indent - 1,
            toolkit_font_ink(face),
            add_modern_row,
            &print
        ))
        return;
    screens::by_list_wrap_text(
        g_open_chat->log,
        text,
        &g_open_chat->owner->system.hat_fonts,
        layout::line_indent,
        font == screens::kDefaultTextColor ? k_no_handle : font
    );
}

const screens::HatFonts* fonts_of(const ChatView* view) {
    return &view->owner->system.hat_fonts;
}

// The list's row `index` when it is of the row class `class_name`.
Window* list_row(screens::ByListBox* list, size_t index, const char* class_name) {
    Window* row = list_box_item(&list->list, static_cast<int32_t>(index));
    return row != nullptr && row->ops->is_class(class_name) ? row : nullptr;
}

// Takes a row out of its list, forgetting it as the selected row, and
// frees it.
void remove_list_row(screens::ByListBox* list, Window* row) {
    list_box_remove(&list->list, row);
    if (list->selected_item == row)
        list->selected_item = nullptr;
    window_destroy(row);
}

/// Keeps the frequency list in step with the chat's.
///
/// A new frequency gets its BYChatRoomListItem, circled while it is the
/// session's, with its count; a joined one shows its new count; a left
/// one's row goes.
///
/// @param user unused context of the chat's output hook
/// @param change how the row changed
/// @param room the row's frequency (what it held, for a removed row)
/// @param index the list row
void chat_room_row(
    void* /*user*/, screens::RowChange change, const screens::ChatRoom* room, size_t index
) {
    ChatView* view = g_open_chat;
    if (view == nullptr)
        return;
    screens::ByListBox* list = view->room_list;
    if (change == screens::RowChange::added) {
        screens::ByChatRoomRow* row = screens::by_chat_room_row_create(
            list,
            room->name,
            room->description,
            room->tag,
            room->room_id,
            room->password_required,
            room->type,
            fonts_of(view),
            &chat_of(view)->session->room_id
        );
        screens::by_chat_room_row_set_count(row, static_cast<uint32_t>(room->population));
        return;
    }
    Window* found = list_row(list, index, screens::kByChatRoomRowClass);
    auto* row = reinterpret_cast<screens::ByChatRoomRow*>(found);
    if (row == nullptr || row->room_id != room->room_id)
        return;
    if (change == screens::RowChange::updated)
        screens::by_chat_room_row_set_count(row, static_cast<uint32_t>(room->population));
    else
        remove_list_row(list, found);
}

/// Keeps the player list in step with the chat's.
///
/// A new member gets a BYPlayerListItem; the rank and latency icons follow
/// the member, and the muzzle and whisper lock pick the name's font at the
/// next draw; a member who left loses the row.
///
/// @param user unused context of the chat's output hook
/// @param change how the row changed
/// @param player the row's member (what it held, for a removed row)
/// @param index the list row
void chat_player_row(
    void* /*user*/, screens::RowChange change, const screens::ChatPlayer* player, size_t index
) {
    ChatView* view = g_open_chat;
    if (view == nullptr)
        return;
    screens::ByListBox* list = view->player_list;
    screens::ByPlayerRow* row = nullptr;
    if (change == screens::RowChange::added) {
        row = screens::by_player_row_create(list, player->name, player->user_id, fonts_of(view));
    } else {
        Window* found = list_row(list, index, screens::kByPlayerRowClass);
        row = reinterpret_cast<screens::ByPlayerRow*>(found);
        if (row == nullptr || row->user_id != player->user_id)
            return;
        if (change == screens::RowChange::removed) {
            remove_list_row(list, found);
            return;
        }
    }
    row->silenced = player->silenced;
    row->whisper_locked = player->whisper_locked;
    if (row->icons.latency_frame != player->latency_icon)
        screens::player_row_set_latency(&row->icons, player->latency_icon);
    if (row->icons.user_frame != player->user_icon)
        screens::player_row_set_user_icon(&row->icons, player->user_icon);
}

void chat_rooms_cleared(void*) {
    if (g_open_chat != nullptr)
        screens::by_list_box_clear(g_open_chat->room_list);
}

void chat_players_cleared(void*) {
    if (g_open_chat != nullptr)
        screens::by_list_box_clear(g_open_chat->player_list);
}

/// Scrolls the frequency list by as few rows as bring a row into view, as the listing's last row
/// does for the session's frequency.
///
/// @param user unused context of the chat's output hook
/// @param index the list row
void chat_show_room(void* /*user*/, size_t index) {
    if (g_open_chat == nullptr)
        return;
    ListBox* list = &g_open_chat->room_list->list;
    const auto row = static_cast<int32_t>(index);
    if (row < list->top)
        list_box_scroll(list, row - list->top);
    else if (row > list->last)
        list_box_scroll(list, row - list->last);
}

void select_list_row(screens::ByListBox* list, int32_t row) {
    list->selected_item = row >= 0 ? list_box_item(&list->list, row) : nullptr;
}

void chat_select_room(void*, int32_t row) {
    if (g_open_chat != nullptr)
        select_list_row(g_open_chat->room_list, row);
}

void chat_select_player(void*, int32_t row) {
    if (g_open_chat != nullptr)
        select_list_row(g_open_chat->player_list, row);
}

void chat_show_player_pane(void*, bool page) {
    if (g_open_chat != nullptr && g_open_chat->player_pane != nullptr)
        player_pane_show(g_open_chat->player_pane, chat_of(g_open_chat)->pane, page);
}

/// Gives the input line the chat's text and the focus, as PAGE's message (300) leaves them.
///
/// @param user unused context of the chat's output hook
void chat_show_input(void* /*user*/) {
    ChatView* view = g_open_chat;
    if (view == nullptr)
        return;
    single_edit_set_text(view->input, chat_of(view)->input);
    if (Desktop* desktop = services().desktop; desktop != nullptr)
        desktop_set_focus(desktop, &view->input->base);
}

/// Runs the input line through the chat: sent as chat, a command, or kept when a whisper could not
/// be sent.
///
/// @param[in,out] view Chat console
/// @return false only for an empty line
bool chat_console_submit(ChatView* view) {
    screens::ChatConsole* chat = chat_of(view);
    oa::boneyards::utf8::copy(
        chat->input, sizeof chat->input, single_edit_text(view->input), screens::kChatLineCharacters
    );
    const bool ran = screens::chat_submit_input(chat);
    single_edit_set_text(view->input, chat->input);
    chat_panes_sync(view);
    return ran;
}

// A typed character, as a single-line entry would take it. Navigation keys
// share that numeric range and are left for the list and the chat line.
bool key_types(int32_t key) {
    if (key < 0 || (key >= key::del && key <= key::system))
        return false;
    return utf8::taken(static_cast<char32_t>(key));
}

// The frequency row's click focuses the list on the way up, after Change
// Frequency has asked for its password. A letter that then arrives here
// belongs in that password, unless the player is already in a text field.
bool deliver_frequency_password(ChatView* view, const Message* msg) {
    FrequencyInfoPane* pane = view->info_pane;
    if (pane == nullptr || pane->needs_password == 0 || pane->password == nullptr ||
        msg == nullptr || !key_types(msg->key))
        return false;
    Window* field = &pane->password->base;
    if (!window_is_shown(field) || field->focused != 0 || field->ops == nullptr ||
        field->ops->key_down == nullptr)
        return false;
    Desktop* desktop = services().desktop;
    Window* focus = desktop != nullptr ? desktop->focus : nullptr;
    if (focus != nullptr && focus->ops != nullptr && focus->ops->is_class != nullptr &&
        (focus->ops->is_class(class_name::single_edit) ||
         focus->ops->is_class(class_name::edit_box)))
        return false;
    services_desktop_focus(field);
    return field->ops->key_down(field, msg);
}

/// Handles the keys the input line does not use.
///
/// While the input line has the focus, Enter runs it and Page Up, Page
/// Down, Up and Down scroll the ChatList; other keys go on to the Main
/// Frame. A password Change Frequency takes a letter that was not typed
/// into a text field.
///
/// @param self the Chat console
/// @param msg key message
/// @return whether the key was consumed
bool chat_console_key_down(Window* self, const Message* msg) {
    ChatView* view = as_chat(self);
    if (msg != nullptr && deliver_frequency_password(view, msg))
        return true;
    const bool typing = view->input->base.focused != 0;
    switch (msg->key) {
    case key::enter:
        return typing ? chat_console_submit(view) : true;
    case key::page_up:
    case key::page_down:
    case key::up:
    case key::down:
        if (typing) {
            Window* log = &view->log->list.base;
            log->ops->key_down(log, msg);
        }
        return true;
    default:
        return self->parent != nullptr && self->parent->ops->key_down(self->parent, msg);
    }
}

// Whether `source` is the console's player, information or create-frequency
// pane.
bool from_player_pane(const ChatView* view, const Window* source) {
    return view->player_pane != nullptr && source == &view->player_pane->pane.base;
}

bool from_info_pane(const ChatView* view, const Window* source) {
    return view->info_pane != nullptr && source == &view->info_pane->pane.base;
}

bool from_create_pane(const ChatView* view, const Window* source) {
    return view->create_pane != nullptr && source == &view->create_pane->pane.base;
}

// The list class's own notice handler, kept when the player list's is replaced.
void (*player_list_notify_base)(Window*, const Notify*) = nullptr;

/// Selects a clicked player and tells the console, including a row already selected.
///
/// The list's selection starts on its first row, and choosing the row already
/// selected reports nothing, so the first player and a second click on the
/// player already shown would never open the pane. A click on either still
/// asks for the profile. The player list does not forward a double click's
/// second press; the first press is the click.
///
/// @param self the player list
/// @param notify control notice; null is ignored
void player_list_on_notify(Window* self, const Notify* notify) {
    if (notify == nullptr || player_list_notify_base == nullptr)
        return;
    ListBox* box = reinterpret_cast<ListBox*>(self);
    const int32_t row = list_box_index_of(box, notify->source);
    const int32_t previous = list_box_selected(box);
    const bool on_page = row >= box->top && row <= box->last;
    if (on_page && notify->source != nullptr && notify->source->ops != nullptr &&
        notify->source->ops->is_class(screens::kByPlayerRowClass))
        reinterpret_cast<screens::ByListBox*>(self)->selected_item = notify->source;
    player_list_notify_base(self, notify);
    if (notify->code == NotifyCode::activated && on_page && previous == row)
        window_notify_parent(self, NotifyCode::activated, 0);
}

/// Makes a click on the player list's selected row open that player's pane.
///
/// @param players the Chat console's player list
void player_list_report_selected_row(screens::ByListBox* players) {
    static WindowOps ops{};
    static bool ready = false;
    if (!ready) {
        ops = *players->list.base.ops;
        player_list_notify_base = ops.on_notify;
        ops.on_notify = player_list_on_notify;
        ready = true;
    }
    players->list.base.ops = &ops;
}

/// Routes the left column's and the panes' notices to the chat by their source.
///
/// A frequency row click opens the frequency's information pane and shows
/// the frequency in it, and a double click goes to it; CREATE shows or hides
/// the create-frequency pane; a player row opens the player's pane;
/// MATCHMAKING opens the Matchmaking dialog. All but the frequency list
/// sound GREEN_BUTTON first. The information pane closing drops the
/// frequency list's selection; the player pane's notices go to the chat's
/// pane messages, where its closing drops the player list's selection; the
/// chat hears of any pane closing.
///
/// @param self the Chat console
/// @param notify control notice
void chat_console_on_notify(Window* self, const Notify* notify) {
    ChatView* view = as_chat(self);
    screens::ChatConsole* chat = chat_of(view);
    const int32_t code = static_cast<int32_t>(notify->code);
    if (notify->source == &view->room_list->list.base) {
        const Window* item = view->room_list->selected_item;
        if (item == nullptr || !item->ops->is_class(screens::kByChatRoomRowClass))
            return;
        const int32_t index = list_box_index_of(&view->room_list->list, item);
        if (index < 0)
            return;
        const uint32_t pending = chat->session->pending_room_id;
        screens::chat_on_control(
            chat, screens::ChatControl::room_list, code, static_cast<size_t>(index)
        );
        chat_panes_sync(view);
        const screens::ChatRoom* room = screens::chat_find_room(chat, chat->room_info_room_id);
        if (chat->session->pending_room_id == pending && chat->room_info_visible &&
            view->info_pane != nullptr && room != nullptr)
            frequency_info_pane_show(view->info_pane, room);
    } else if (notify->source == view->create_button) {
        play_sound(view, k_green_button);
        screens::chat_on_control(chat, screens::ChatControl::create_button, code, 0);
        chat_panes_sync(view);
    } else if (from_info_pane(view, notify->source)) {
        if (code == pane_notify::closed) {
            chat->room_info_visible = false;
            chat->selected_room = -1;
            select_list_row(view->room_list, -1);
        }
    } else if (from_player_pane(view, notify->source)) {
        screens::chat_handle_pane_message(chat, code, 0, nullptr);
    } else if (from_create_pane(view, notify->source)) {
        if (code == pane_notify::closed)
            chat->create_dialog.visible = false;
    } else if (notify->source == &view->player_list->list.base) {
        play_sound(view, k_green_button);
        const int32_t index = list_box_selected(&view->player_list->list);
        screens::chat_on_control(
            chat, screens::ChatControl::player_list, code, static_cast<size_t>(index)
        );
        chat_panes_sync(view);
    } else if (view->matchmaking != nullptr && notify->source == view->matchmaking) {
        play_sound(view, k_green_button);
        if (gated_feature_shown(
                ReleaseFeature::matchmaking, view->owner->system.matchmaking_enabled
            ))
            matchmaking_dialog_open(view->owner, k_matchmaking_settings);
    }
}

// The console's own image: keyed, with the separators; the one closing the
// left column over MATCHMAKING only when that is built.
void chat_console_draw_rules(Window* self, bool matchmaking) {
    namespace l = layout;
    const Handle frame =
        image_create_blank(self->width, self->height, screens::kWidgetKey, screens::kWidgetKey);
    window_set_state_image(self, k_frame_state, frame);
    Surface surface{};
    if (image_surface(frame, &surface)) {
        (void)present::draw_clipped_line(
            &surface, l::column_edge, l::column_top, l::column_edge, self->height - 4, l::rule_color
        );
        (void)present::draw_clipped_line(
            &surface,
            l::column_edge,
            l::input_rule_y,
            self->width - 1,
            l::input_rule_y,
            l::rule_color
        );
        (void)present::draw_clipped_line(
            &surface, 0, l::players_rule_y, l::column_edge, l::players_rule_y, l::rule_color
        );
        if (matchmaking)
            (void)present::draw_clipped_line(
                &surface, 0, l::input_rule_y, l::column_edge, l::input_rule_y, l::rule_color
            );
    }
    image_release(frame);
}

screens::ByListBox* add_list(
    Window* self, bool forward_changes, int32_t dy, int32_t width, int32_t height, const char* name
) {
    screens::ByListBox* list = screens::by_list_box_create(false, forward_changes);
    window_set_bounds(&list->list.base, self->x, self->y + dy, width, height, self, true);
    if (name != nullptr && name[0] != '\0')
        window_set_name(&list->list.base, name);
    return list;
}

// How far the entry line grows above its 14-pixel box. The log under an open
// pane keeps at least one pixel of height.
int32_t entry_grow() {
    if (!modern_chat_on())
        return 0;
    const ModernChatLine* line = modern_chat_line();
    if (line == nullptr)
        return 0;
    int32_t grow = line->entry_height - layout::input_height;
    if (grow < 0)
        grow = 0;
    const int32_t room = layout::log_pane_height - 1;
    if (grow > room)
        grow = room;
    return grow;
}

// A pane is open when it is shown and parented to this console.
bool pane_shown(const InfoPane* pane, const Window* console) {
    return pane != nullptr && pane->base.visible != 0 && pane->base.parent == console;
}

bool chat_pane_open(const ChatView* view) {
    const Window* console = &view->base;
    const InfoPane* player = view->player_pane != nullptr ? &view->player_pane->pane : nullptr;
    const InfoPane* create = view->create_pane != nullptr ? &view->create_pane->pane : nullptr;
    const InfoPane* info = view->info_pane != nullptr ? &view->info_pane->pane : nullptr;
    return pane_shown(player, console) || pane_shown(create, console) || pane_shown(info, console);
}

// The panes move the ChatList by these heights, so a later open or close
// keeps the room the entry line took.
void remember_log_layout(ChatView* view, int32_t grow) {
    if (view->log == nullptr)
        return;
    const InfoPaneLayout panes{
        &view->log->list,
        layout::log_y,
        layout::log_height - grow,
        layout::log_pane_y,
        layout::log_pane_height - grow,
        nullptr,
        0,
        0
    };
    if (view->player_pane != nullptr)
        info_pane_set_layout(&view->player_pane->pane, panes);
    if (view->create_pane != nullptr)
        info_pane_set_layout(&view->create_pane->pane, panes);
    if (view->info_pane != nullptr)
        info_pane_set_layout(&view->info_pane->pane, panes);
}

// Existing rows keep their breaks. They grow or shrink to the current row,
// and their labels follow the faces in use.
void restyle_log(ChatView* view) {
    if (view->log == nullptr)
        return;
    ListBox* list = &view->log->list;
    const bool on = modern_chat_on();
    const ModernChatLine* line = modern_chat_line();
    const int32_t height = on && line != nullptr ? line->row_height : screens::kByRowHeight;
    for (int32_t i = 0; i < list->count; ++i) {
        Window* item = list_box_item(list, i);
        if (item == nullptr)
            continue;
        auto* row = reinterpret_cast<screens::ByTextRow*>(item);
        window_set_size(&row->base, row->base.width, height);
        if (row->label == nullptr)
            continue;
        window_set_size(row->label, row->label->width, height);
        if (on)
            modern_chat_mark(row->label, toolkit_font_ink(row->label->font));
        else
            modern_chat_unmark(row->label);
    }
    if (list->base.ops != nullptr && list->base.ops->refresh != nullptr)
        list->base.ops->refresh(&list->base);
}

// Moves a window's children by dy without the parent's movement clamp.
void move_children_by(Window* self, int32_t dy) {
    if (dy == 0)
        return;
    for (int32_t i = 0; i < self->children.count; ++i) {
        Window* child = self->children.items[i];
        if (child == nullptr)
            continue;
        child->y += dy;
        move_children_by(child, dy);
    }
}

// Puts the ChatList and the entry line on the modern faces, or back on the
// Boneyards' fonts. The prompt stays where it was drawn. The list's top is
// assigned directly: moving it would clamp a list that fills its parent.
void place_chat_text(ChatView* view) {
    if (view == nullptr || view->log == nullptr || view->input == nullptr)
        return;
    const int32_t grow = entry_grow();
    // A closed console is not shown, and the list only bottom-aligns rows it
    // considers shown. Layout while the console counts as shown, then put
    // its visibility back.
    const uint8_t shown = view->base.visible;
    view->base.visible = 1;
    const bool open = chat_pane_open(view);
    ListBox* log = &view->log->list;
    int32_t log_height = (open ? layout::log_pane_height : layout::log_height) - grow;
    if (log_height < 1)
        log_height = 1;
    log->base.y = view->base.y + (open ? layout::log_pane_y : layout::log_y);
    window_set_height(&log->base, log_height);
    remember_log_layout(view, grow);
    restyle_log(view);
    view->base.visible = shown;

    SingleEdit* input = view->input;
    if (modern_chat_on())
        modern_chat_mark(&input->base, toolkit_font_ink(input->base.font));
    else
        modern_chat_unmark(&input->base);
    const int32_t rest_y = view->base.y + layout::input_y;
    input->rest_y = rest_y;
    input->rest_height = layout::input_height;
    const int32_t old_y = input->base.y;
    const int32_t old_height = input->base.height;
    const int32_t previous_selection = input->selection_height;
    const int32_t box = layout::input_height + grow;
    input->base.y = rest_y - grow;
    input->base.height = box;
    move_children_by(&input->base, input->base.y - old_y);
    single_edit_set_text(input, single_edit_text(input));
    if (modern_chat_on() && (input->base.y != rest_y - grow || input->base.height != box)) {
        const int32_t dy = (rest_y - grow) - input->base.y;
        input->base.y = rest_y - grow;
        input->base.height = box;
        move_children_by(&input->base, dy);
    }
    // 0x11 is the Boneyards entry's grown height. A modern height is neither
    // that nor 14, and the Boneyards fit would leave it in place.
    if (previous_selection == old_height || previous_selection == layout::input_height ||
        previous_selection == 0x11)
        input->selection_height = input->base.height;
}

/// Builds the Chat console hidden when its bounds are set.
///
/// Adds its image with the separators, the ChatList (100 lines,
/// bottom-aligned, a log), the "Enter Chat >>" prompt and the input line
/// (small font, scrolling, 243 characters, a 14-pixel dark red selection),
/// the "CHAT FREQUENCIES" title over the frequency list, CREATE, "PLAYERS"
/// over the player list and, when Matchmaking is on screen, "MATCHMAKING"
/// beside its Minimize_Button under a shorter player list. With modern fonts
/// on, the log and the entry line then take the player's Text size, and the
/// log makes room for the taller entry.
///
/// @param self the Chat console
void chat_console_build(Window* self) {
    namespace l = layout;
    ChatView* view = as_chat(self);
    const screens::HatFonts& fonts = view->owner->system.hat_fonts;
    const bool matchmaking =
        gated_feature_shown(ReleaseFeature::matchmaking, view->owner->system.matchmaking_enabled);
    chat_console_draw_rules(self, matchmaking);

    view->log = screens::by_list_box_create(false, false);
    ListBox* log = &view->log->list;
    window_create(
        &log->base,
        k_log_name,
        self->x + l::log_x,
        self->y + l::log_y,
        l::log_width,
        l::log_height,
        self,
        true
    );
    log->bottom_align = 1;
    log->capacity = l::log_capacity;
    log->log_mode = 1;
    screens::lobby_label_create(
        self,
        l::prompt_x,
        l::prompt_y,
        "Enter Chat >>",
        fonts.small_dim,
        k_align_bottom_left,
        k_anchor_bottom_left,
        l::prompt_width
    );

    SingleEdit* input = single_edit_create();
    view->input = input;
    window_set_bounds(
        &input->base,
        self->x + l::input_x,
        self->y + l::input_y,
        self->width - l::input_x,
        l::input_height,
        self,
        true
    );
    window_set_font_handle(&input->base, fonts.small);
    input->scroll = 1;
    single_edit_set_selection_height(input, l::input_height);
    single_edit_set_max_length(input, l::input_max);
    single_edit_set_selection_color(
        input, l::selection_r, l::selection_g, l::selection_b, l::selection_a
    );
    window_set_name(&input->base, "Chat entry");

    screens::lobby_label_create(
        self,
        0,
        l::title_y,
        "CHAT FREQUENCIES",
        fonts.big,
        k_align_centre,
        k_anchor_top_left,
        l::title_width
    );
    view->room_list =
        add_list(self, true, l::rooms_y, l::rooms_width, l::rooms_height, "Chat rooms");
    view->create_button = button_create();
    screens::function_button_setup(
        view->create_button, false, l::create_x, l::create_y, "CREATE", self
    );
    window_set_state(view->create_button, button_state::unpressed);
    screens::lobby_label_create(
        self,
        0,
        l::players_title_y,
        "PLAYERS",
        fonts.big,
        k_align_centre,
        k_anchor_top_left,
        l::players_title_width
    );
    view->player_list = add_list(
        self,
        false,
        l::players_y,
        l::players_width,
        matchmaking ? l::players_height_matchmaking : l::players_height,
        "Chat players"
    );
    player_list_report_selected_row(view->player_list);
    if (matchmaking) {
        screens::lobby_label_create(
            self,
            0,
            l::matchmaking_y,
            "MATCHMAKING",
            fonts.big,
            k_align_centre,
            k_anchor_top_left,
            l::matchmaking_width
        );
        view->matchmaking = button_create();
        window_set_bounds(
            view->matchmaking,
            self->x + l::minimize_x,
            self->y + l::matchmaking_y,
            l::minimize_width,
            l::minimize_height,
            self,
            true
        );
        view->matchmaking->ops->bind_images(
            view->matchmaking, screens::kWidgetsGaf, k_minimize_sequence, 0, screens::kWidgetKey
        );
        // The dialog is already named Matchmaking. Naming this button that
        // too makes a lookup of the dialog find the button, so the dialog
        // never opens and the focus order and the queue never see it.
        window_set_name(view->matchmaking, k_minimize_sequence);
    }
    place_chat_text(view);
    self->visible = 0;
}

/// Follows the frequency being entered with CREATE while the console is shown.
///
/// CREATE is disabled while a frequency change is pending and enabled again
/// once none is; its face is only written when that changes it.
///
/// @param self the Chat console
void chat_console_pulse(Window* self) {
    if (!window_is_shown(self))
        return;
    ChatView* view = as_chat(self);
    Window* create = view->create_button;
    const bool disabled = create->states.current == button_state::disabled;
    if (chat_of(view)->session->pending_room_id != 0) {
        if (!disabled)
            window_set_state(create, button_state::disabled);
    } else if (disabled) {
        window_set_state(create, button_state::unpressed);
    }
}

// Gives the Main Frame back the panes this console holds, hidden and making
// room for nothing, so they outlive it.
void chat_console_release_panes(Window* self) {
    ChatView* view = as_chat(self);
    for (InfoPane* pane : view->frame->panes) {
        if (pane == nullptr || pane->base.parent != self)
            continue;
        pane->base.visible = 0;
        info_pane_set_layout(pane, InfoPaneLayout{});
        window_set_parent(&pane->base, &view->frame->base);
    }
}

/// Stops being the console the chat prints to and frees the window.
///
/// The chat's link to its console is cleared only while it still names this
/// console, as a replaced console is destroyed after its successor is built.
///
/// @param self the Chat console
void chat_console_destroy(Window* self) {
    if (g_open_chat == as_chat(self))
        g_open_chat = nullptr;
    console_window_free(self);
}

WindowOps make_chat_ops() {
    WindowOps ops = console_panel_ops();
    ops.destroy = chat_console_destroy;
    ops.on_destroy = chat_console_release_panes;
    ops.on_bounds = chat_console_build;
    ops.pulse = chat_console_pulse;
    ops.on_notify = chat_console_on_notify;
    ops.key_down = chat_console_key_down;
    return ops;
}

const WindowOps k_chat_ops = make_chat_ops();

} // namespace

void chat_console_open(Window* self) {
    namespace l = layout;
    ChatView* view = as_chat(self);
    const int32_t grow = entry_grow();
    const InfoPaneLayout panes{
        &view->log->list,
        l::log_y,
        l::log_height - grow,
        l::log_pane_y,
        l::log_pane_height - grow,
        nullptr,
        0,
        0
    };
    view->player_pane = reinterpret_cast<PlayerPaneView*>(
        main_frame_pane(view->frame, PaneKind::player, self, panes)
    );
    view->create_pane = reinterpret_cast<CreateFrequencyPane*>(
        main_frame_pane(view->frame, PaneKind::create_frequency, self, panes)
    );
    view->info_pane = reinterpret_cast<FrequencyInfoPane*>(
        main_frame_pane(view->frame, PaneKind::frequency_info, self, panes)
    );
    ConsoleNavButton* button = chat_button_of(view);
    window_set_state_image(&button->base, button_state::unpressed, button->lit);
    screens::chat_open_on_planet(chat_of(view));
    if (Desktop* desktop = services().desktop; desktop != nullptr)
        desktop_set_focus(desktop, &view->input->base);
    self->visible = 1;
}

void chat_console_close(Window* self) {
    ChatView* view = as_chat(self);
    ConsoleNavButton* button = chat_button_of(view);
    window_set_state_image(&button->base, button_state::unpressed, button->normal);
    screens::chat_close(chat_of(view));
    chat_panes_sync(view);
    self->visible = 0;
}

Window* chat_console_create(
    StatusWidget* owner, Window* frame, int32_t x, int32_t y, int32_t width, int32_t height
) {
    auto* view = reinterpret_cast<ChatView*>(console_window_alloc(sizeof(ChatView), &k_chat_ops));
    auto* main_frame = reinterpret_cast<MainFrame*>(frame);
    view->frame = main_frame;
    view->chat_button = main_frame->console_buttons[static_cast<int32_t>(ConsoleButton::chat)];
    view->owner = owner;
    g_open_chat = view;
    screens::BoneyardsScreens* nav = owner->navigation;
    screens::boneyards_open(nav, screens::ByScreen::chat_frequencies);
    screens::ChatUi& ui = nav->chat.ui;
    ui.print_line = chat_print_line;
    ui.show_room = chat_show_room;
    ui.room_row = chat_room_row;
    ui.player_row = chat_player_row;
    ui.rooms_cleared = chat_rooms_cleared;
    ui.players_cleared = chat_players_cleared;
    ui.select_room = chat_select_room;
    ui.pane_changed = chat_show_player_pane;
    ui.select_player = chat_select_player;
    ui.show_input = chat_show_input;
    const screens::HatFonts& fonts = owner->system.hat_fonts;
    screens::ChatSession* session = nav->chat.session;
    session->system_font = fonts.small_green;
    session->rank_0x15_font = fonts.small_orange;
    session->arm_font = fonts.small_blue;
    session->core_font = fonts.small_red;
    window_create(&view->base, k_console_name, x, y, width, height, frame, true);
    return &view->base;
}

ChatView* console_open_chat() {
    return g_open_chat;
}

void chat_set_modern_text(bool modern_fonts, int32_t text_size) {
    modern_chat_set(modern_fonts, text_size);
    const bool on = modern_chat_on();
    const ModernChatLine* line = modern_chat_line();
    const int32_t pixel = line != nullptr ? line->pixel_size : 0;
    const int32_t entry = line != nullptr ? line->entry_height : layout::input_height;
    const int32_t row = line != nullptr ? line->row_height : screens::kByRowHeight;
    // The result, not the request: a size whose faces could not be opened is
    // tried again on the next change.
    static bool applied = false;
    static bool applied_on = false;
    static int32_t applied_pixel = 0;
    static int32_t applied_entry = 0;
    static int32_t applied_row = 0;
    if (applied && applied_on == on && applied_pixel == pixel && applied_entry == entry &&
        applied_row == row)
        return;
    applied = true;
    applied_on = on;
    applied_pixel = pixel;
    applied_entry = entry;
    applied_row = row;
    if (g_open_chat != nullptr)
        place_chat_text(g_open_chat);
}

} // namespace oa::boneyards::console
