// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Contents consoles: Help, Ladders and News. A table of contents read from a
// .def file on the left, and an HTML view on the right showing the page of
// the selected headline (the first one when the contents arrive).
//
// Documents are named by locators: "#Remote#" paths on the content server
// of the console's kind, http:// URLs, or files relative to the game
// directory. Local files are handled at once; remote ones are downloaded to
// temporary files that arrive through the service's file_arrived event at
// the console last built or opened, and are deleted once read. Built here:
// the console record, its build, its notifications, locator resolution and the
// handling of arrived files, the contents rows with the category arrows and
// the light title of the headline whose page is shown, the light title of the
// chosen category while the keyboard ring shows, the ContentsList and
// its scroll bar, the open and close slots that light the console's Main
// Frame button, and the Ladders console's name search. The console shows the
// system record's shared contents image: a keyed blank with one column rule.
//
// Opening or closing a category builds the rows again. The rows in view stay
// where they were, every row holds one title inside it, and the scroll bar
// shows along the list's right edge only while rows are out of view, its
// thumb as long as the share of the rows in view.

#include "oa/boneyards/utf8.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "console_state.hpp"
#include "html_view.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/screens/contents.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
#include "widgets.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/system.hpp"

namespace oa::boneyards::console {

// A test names this. Empty means the pages shipped beside the program.
std::string g_help_override_directory;

void boneyards_console_set_help_override_directory(const char* directory) {
    g_help_override_directory = directory != nullptr ? directory : "";
}

namespace {

constexpr std::size_t k_locator_bytes = 0x400;
constexpr std::size_t k_row_text_bytes = 0x100;

constexpr const char* k_help_contents = "./boneyards/Help/Help.def";
constexpr const char* k_ladders_contents = "./boneyards/Ladders/Ladders.def";
constexpr const char* k_news_contents = "#Remote#/boneyards/ta/news/news.def";
constexpr const char* k_waiting_text =
    "<HTML><BODY leftmargin=5 topmargin=10>\n<FONT face=\"Haettenschweiler\" size=\"+2\">Waiting "
    "for contents list to load.\n</FONT></BODY></HTML>\n";
constexpr const char* k_list_sound = "GREEN_BUTTON";

// The Ladders console's name search: the "Name" caption above the entry
// (its bottom-left corner at the given point), the framed scrolling entry
// of 15 characters and SEARCH, placed in the console, over the lower rows
// of the ContentsList. A search fetches the ladder's index for the name's
// first letter, which names the page showing the name.
constexpr int32_t k_search_caption_x = 6;
constexpr int32_t k_search_caption_y = 0x100;
constexpr int32_t k_search_caption_alignment = 6;
constexpr int32_t k_search_caption_anchor = 2;
constexpr int32_t k_search_entry_x = 6;
constexpr int32_t k_search_entry_y = 0x104;
constexpr int32_t k_search_entry_width = 0x70;
constexpr int32_t k_search_entry_max = 0xf;
constexpr int32_t k_search_button_x = 0xc;
constexpr int32_t k_search_button_y = 0x11d;

// Build layout: the "TOC" title, the ContentsList (absolute position) and
// the view, inset from the console's right and bottom edges.
constexpr int32_t k_title_dy = 6;
constexpr int32_t k_title_width = 0x7c;
constexpr int32_t k_list_x = 0x17;
constexpr int32_t k_list_y = 0x93;
constexpr int32_t k_list_width = 0x7c;
constexpr int32_t k_list_height = 0x121;
constexpr int32_t k_view_dx = 0x81;
constexpr int32_t k_view_dy = 5;
constexpr int32_t k_view_width_inset = 0x83;
constexpr int32_t k_view_height_inset = 10;
// The shown state carries the shared contents image.
constexpr int32_t k_image_state = 1;

// ContentsList scroll bar: the HTMLSlider class, 0x11 wide with its left edge
// 0x12 in from the list's right edge, as tall as the list. Its arrows are 9
// high. Its thumb is as long as the share of the rows in view, never shorter
// than the scrollbar art's long thumb (0x1e), in whole pairs of pixels so it
// meets both ends of the track.
constexpr const char* k_list_slider_name = "slider";
constexpr const char* k_list_thumb_name = "sbutton";
constexpr int32_t k_list_slider_inset = 0x12;
constexpr int32_t k_list_slider_width = 0x11;
constexpr int32_t k_list_arrow_height = 9;
constexpr int32_t k_list_thumb_min = 0x1e;
constexpr uint8_t k_list_slider_key = 9;

// Contents rows: 0x11 high, as wide as the list less its scroll bar. Every
// title, a category's or a headline's, starts 0xe in and ends at the row's
// right edge, where the 1998 console draws them all.
constexpr int32_t k_row_height = 0x11;
constexpr int32_t k_title_dx = 0xe;
// Stored in every row's reserved_after_list.
constexpr int32_t k_row_reserved_after_list = 3;

// Category arrow: an 11-pixel Static at the row's left edge, 3 pixels below
// its top, which centres the arrow on the row's title.
constexpr const char* k_category_icon_name = "CategoryIcon";
constexpr int32_t k_category_icon_dy = 3;
constexpr int32_t k_category_icon_size = 0xb;
constexpr const char* k_arrow_gaf = "Widgets.Gaf";
constexpr const char* k_arrow_open = "Icon_Arrow_Down";
constexpr const char* k_arrow_closed = "Icon_Arrow_Up";

enum class RowKind : int32_t {
    headline = 7,
    category = 8,
};

// What names a downloaded temporary file, which is deleted once handled.
constexpr const char* k_download_mark = "0BY";

/// Drops a window's background colour so the green console layer shows through it.
///
/// @param[in,out] w window whose colour is released
void drop_window_colour(Window* w) {
    color_release(w->color);
    w->color = k_no_handle;
}

// Category and headline rows share their first fields.
struct ContentsRow {
    Window base;
    RowKind kind;
    screens::ByListBox* list;
    int32_t reserved_after_list;    // always 3; nothing reads it
    int32_t row_height;             // always 0x11
    uint8_t highlight;              // ? title colour follows the selection
    char text[k_row_text_bytes];    // category name or headline title
    uint8_t expanded;               // category: headlines shown
    int32_t category;               // category: record in the console's contents
    Window* icon;                   // category: the arrow
    char locator[k_row_text_bytes]; // headline: document to open
    Window* label;
    // The system record's fonts the title takes.
    const screens::HatFonts* fonts;
    // What a program driving the game reads. A category leads with "+ " while
    // its headlines are hidden and "- " while they are shown. A headline is
    // the title. The label the player sees stays `text`.
    char listed[k_row_text_bytes + 4]{};
};

// A contents console: the list of categories and headlines and the page view.
struct ContentsConsole {
    Window base;
    MainFrame* frame;
    Window* console_button; // the Main Frame button of this console
    HtmlView* view;
    screens::ByListBox* list; // ContentsList
    char contents_locator[k_locator_bytes];
    char page_locator[k_locator_bytes];
    // The page in the view as the contents or the link that opened it name it,
    // before a remote locator is resolved; its headline is highlighted.
    char shown_locator[k_locator_bytes]{};
    int32_t server_type; // content server kind, 1 on Ladders
    screens::Contents contents;
    StatusWidget* owner;
    // The Ladders console's search: the name entry, SEARCH, the index being
    // fetched and the name searched for.
    SingleEdit* search_entry;
    Window* search_button;
    char search_locator[k_locator_bytes];
    char search_name[k_locator_bytes];
};

ContentsConsole* as_console(Window* w) {
    return reinterpret_cast<ContentsConsole*>(w);
}

void copy_locator(char* out, const char* text) {
    copy_text(out, k_locator_bytes, text);
}

/// Turns a contents path into a game path by dropping each leading "./".
///
/// 3.1c opens these paths relative to the game directory; the console's reader takes game paths.
///
/// @param path the path as the contents give it
/// @return the same path past its leading "./" parts
const char* game_path(const char* path) {
    while (path[0] == '.' && (path[1] == '/' || path[1] == '\\'))
        path += 2;
    return path;
}

char lower_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool same_ascii(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower_ascii(a[i]) != lower_ascii(b[i]))
            return false;
    return true;
}

/// Tests whether two locators name the same document: the same text past any leading "./", with
/// no regard to the case of ASCII letters or to the kind of slash.
///
/// @param a a locator
/// @param b another locator
/// @return true when they name the same document
bool same_locator(const char* a, const char* b) {
    a = game_path(a);
    b = game_path(b);
    for (; *a != '\0' && *b != '\0'; ++a, ++b) {
        const char left = *a == '\\' ? '/' : lower_ascii(*a);
        const char right = *b == '\\' ? '/' : lower_ascii(*b);
        if (left != right)
            return false;
    }
    return *a == *b;
}

/// The file name of a Help page path, or false when `path` is not one.
///
/// @param path a contents path, possibly with a leading "./"
/// @param[out] name the page's file name, with no folder
/// @return true for boneyards/Help/something
bool help_page_name(const char* path, std::string* name) {
    if (path == nullptr || name == nullptr)
        return false;
    std::string relative(game_path(path));
    for (char& c : relative)
        if (c == '\\')
            c = '/';
    std::string parts[3];
    std::size_t count = 0;
    std::size_t start = 0;
    while (start <= relative.size() && count < 3) {
        const std::size_t end = relative.find('/', start);
        const std::string part =
            relative.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? relative.size() + 1 : end + 1;
        if (part.empty() || part == ".")
            continue;
        parts[count] = part;
        ++count;
    }
    if (start <= relative.size() || count != 3)
        return false;
    if (!same_ascii(parts[0], "boneyards") || !same_ascii(parts[1], "Help"))
        return false;
    if (parts[2].empty() || parts[2] == ".." || parts[2].find('/') != std::string::npos)
        return false;
    *name = parts[2];
    return true;
}

/// Reads one whole file. False when it cannot be opened.
///
/// @param file path to read
/// @param[out] out receives the bytes
/// @return true when the file was read
bool read_whole_file(const std::filesystem::path& file, std::vector<uint8_t>* out) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error) || error)
        return false;
    std::FILE* handle = oa::platform::open_file(file, "rb");
    if (handle == nullptr)
        return false;
    std::vector<uint8_t> bytes;
    uint8_t chunk[0x1000];
    for (std::size_t n = 0; (n = std::fread(chunk, 1, sizeof chunk, handle)) > 0;)
        bytes.insert(bytes.end(), chunk, chunk + n);
    std::fclose(handle);
    *out = std::move(bytes);
    return true;
}

/// Reads a shipped Help page, when one replaces the expansion's page.
///
/// The folder a test named is first. Otherwise the page beside the program,
/// under boneyards/Help, is the one the packages carry. A missing file is
/// left to the caller's reader, which finds the expansion's copy.
///
/// @param path contents path of a Help page
/// @param[out] out receives the page
/// @return true when a shipped page was read
bool read_shipped_help(const char* path, std::vector<uint8_t>* out) {
    std::string name;
    if (!help_page_name(path, &name))
        return false;
    std::filesystem::path file;
    if (!g_help_override_directory.empty()) {
        file = std::filesystem::path(g_help_override_directory) / name;
    } else {
        const std::string program = oa::platform::program_directory();
        if (program.empty())
            return false;
        file = std::filesystem::path(program) / "boneyards" / "Help" / name;
    }
    return read_whole_file(file, out);
}

// A downloaded document is named by its absolute path, which the game's file
// reader does not reach; it is read directly.
bool read_game_file(StatusWidget* owner, const char* path, std::vector<uint8_t>* out) {
    if (!std::filesystem::path(path).is_absolute()) {
        if (read_shipped_help(path, out))
            return true;
        const FileReader& files = owner->config.files;
        return files.read != nullptr && files.read(files.user, game_path(path), out);
    }
    std::FILE* file = oa::platform::open_file(path, "rb");
    if (file == nullptr)
        return false;
    out->clear();
    uint8_t chunk[0x1000];
    for (std::size_t n = 0; (n = std::fread(chunk, 1, sizeof chunk, file)) > 0;)
        out->insert(out->end(), chunk, chunk + n);
    std::fclose(file);
    return true;
}

void play_list_sound(ContentsConsole* console) {
    const MainMenuHost& host = console->owner->config.host;
    if (host.play_sound != nullptr)
        host.play_sound(host.user, k_list_sound);
}

// ---- .def contents files ----------------------------------------------------

// Platform replacement for the Rover interface's contents reader: each
// "{[Name<T:key=value>...]}" record becomes one contents field for the
// console's record handler.
void read_contents(ContentsConsole* console, const char* path) {
    std::vector<uint8_t> bytes;
    if (!read_game_file(console->owner, path, &bytes))
        return;
    std::string text(bytes.begin(), bytes.end());
    std::size_t at = 0;
    while ((at = text.find("{[", at)) != std::string::npos) {
        const std::size_t end = text.find("]}", at);
        if (end == std::string::npos)
            break;
        std::vector<std::pair<std::string, std::string>> values;
        std::size_t field = text.find('<', at);
        while (field != std::string::npos && field < end) {
            const std::size_t colon = text.find(':', field);
            const std::size_t equals = text.find('=', field);
            const std::size_t close = text.find('>', field);
            if (colon == std::string::npos || equals == std::string::npos ||
                close == std::string::npos || close > end || equals > close)
                break;
            values.emplace_back(
                text.substr(colon + 1, equals - colon - 1),
                text.substr(equals + 1, close - equals - 1)
            );
            field = text.find('<', close);
        }
        auto value = [&](const char* key) -> const char* {
            for (const auto& [name, v] : values)
                if (name == key)
                    return v.c_str();
            return nullptr;
        };
        screens::ContentsField record{};
        record.kind = value("command");
        record.id = value("id");
        record.name = value("name");
        record.order = value("sort");
        record.parent = value("category");
        record.title = value("headline");
        record.locator = value("file");
        screens::contents_apply_field(&console->contents, &record);
        at = end + 2;
    }
}

// ---- ContentsList -----------------------------------------------------------

// The ContentsList is the Boneyards list box record: its slider inset flag
// and its flag passing "changed" notifications are not set by these
// consoles, so the slider is not inset and "changed" is dropped; its
// selected row is the one whose title the rows draw light.

/// Raises the ContentsList in place of its scroll bar, which a press on the bar or its buttons
/// raises.
///
/// The list lays out every child after its first as a row, so the bar stays its first child.
///
/// @param self the ContentsList's scroll bar
void list_slider_raise(Window* self) {
    Window* list = self->parent;
    if (list != nullptr)
        list->ops->raise(list);
}

/// Returns the ContentsList's scroll bar class: the HTMLSlider's, raised with its list.
///
/// @param html_slider the HTMLSlider class
/// @return the class, made on the first call
const WindowOps* list_slider_ops(const WindowOps& html_slider) {
    static const WindowOps ops = [&html_slider] {
        WindowOps made = html_slider;
        made.raise = list_slider_raise;
        return made;
    }();
    return &ops;
}

/// Hides the ContentsList's selection fill, drops its background colour and adds an HTMLSlider as
/// its scroll bar, along its right edge.
///
/// The slider paints its own track, and is hidden while every row is in view. With no background
/// colour the green console layer shows through the list, and through the keyed pixels of the
/// track.
///
/// @param self the ContentsList
void contents_list_create_slider(Window* self) {
    drop_window_colour(self);
    auto* list = reinterpret_cast<ListBox*>(self);
    list->no_highlight = 1;
    list->keep_slider = 1;
    Slider* slider = html_slider_create();
    slider->base.ops = list_slider_ops(*slider->base.ops);
    window_create(
        &slider->base,
        k_list_slider_name,
        self->width - k_list_slider_inset + self->x,
        self->y,
        k_list_slider_width,
        self->height,
        self,
        true
    );
    k_list_box_ops.refresh(self);
}

/// Selects a clicked row, telling the console again when it was already selected.
///
/// "changed" notices are dropped; the slider's go to the toolkit list box.
///
/// @param self the ContentsList
/// @param notify control notice; may be null
void contents_list_on_notify(Window* self, const Notify* notify) {
    if (notify == nullptr || notify->code == NotifyCode::changed)
        return;
    auto* contents_list = reinterpret_cast<screens::ByListBox*>(self);
    ListBox* list = &contents_list->list;
    const int32_t index = list_box_index_of(list, notify->source);
    if (index < 0 || notify->source->ops->is_class(class_name::slider)) {
        k_list_box_ops.on_notify(self, notify);
        return;
    }
    const int32_t previous = list_box_selected(list);
    contents_list->selected_item = list_box_item(list, index);
    list_box_select(list, index);
    if (previous == index)
        window_notify_parent(self, NotifyCode::activated, 0);
}

bool contents_list_key_down(Window* self, const Message* msg);

WindowOps make_list_ops() {
    WindowOps ops = k_list_box_ops;
    ops.on_bounds = contents_list_create_slider;
    ops.on_notify = contents_list_on_notify;
    ops.key_down = contents_list_key_down;
    return ops;
}

const WindowOps k_contents_list_ops = make_list_ops();

/// Makes one face of the scroll bar's thumb `height` pixels tall: the art's top half, its middle
/// row repeated, then its bottom half, each column where the art draws it.
///
/// A face shorter than the art keeps the art's two ends and drops its middle.
///
/// @param art the thumb's face in one state
/// @param width the thumb's width in pixels
/// @param height the thumb's new height in pixels
/// @return the new face, or k_no_handle when the art's pixels cannot be read
Handle stretched_thumb_face(Handle art, int32_t width, int32_t height) {
    Surface from{};
    if (!image_surface(art, &from) || from.pixels == nullptr || from.height < 1)
        return k_no_handle;
    const Handle face = image_create_blank(width, height, k_list_slider_key, k_list_slider_key);
    Surface to{};
    if (!image_surface(face, &to) || to.pixels == nullptr) {
        image_release(face);
        return k_no_handle;
    }
    const int32_t kept = from.height < height ? from.height : height;
    const int32_t head = kept / 2;
    const int32_t tail = kept - head;
    for (int32_t y = 0; y < height; ++y) {
        int32_t row = head;
        if (y < head)
            row = y;
        else if (y >= height - tail)
            row = from.height - (height - y);
        const uint8_t* source = from.pixels + static_cast<std::ptrdiff_t>(row) * from.pitch;
        uint8_t* target = to.pixels + static_cast<std::ptrdiff_t>(y) * to.pitch;
        for (int32_t x = 0; x < from.width; ++x) {
            const int32_t at = x - from.origin_x;
            if (at >= 0 && at < to.width)
                target[at] = source[x];
        }
    }
    return face;
}

/// Sizes the ContentsList's thumb to the share of the rows in view and places it again.
///
/// The thumb runs along the track between the arrows: as long as the track times the rows in view
/// over all the rows, rounded down to whole pairs of pixels, and never shorter than the art's long
/// thumb. Its faces are drawn again at that length.
///
/// @param[in,out] list the ContentsList
void contents_list_fit_thumb(ListBox* list) {
    Window* bar = window_find_named(&list->base, k_list_slider_name, nullptr);
    Window* thumb = bar != nullptr ? window_find_named(bar, k_list_thumb_name, nullptr) : nullptr;
    if (thumb == nullptr || list->count < 1)
        return;
    const int32_t track = bar->height - 2 * k_list_arrow_height;
    const int32_t fits = (list->base.height - 1) / k_row_height;
    const int32_t shown = list->count < fits ? list->count : fits;
    int32_t height = track * shown / list->count;
    height -= height % 2;
    if (height < k_list_thumb_min)
        height = k_list_thumb_min;
    if (height > track)
        height = track;
    if (thumb->height != height) {
        for (int32_t state = 0; state < thumb->states.count; ++state) {
            const Handle face =
                stretched_thumb_face(thumb->states.images[state], thumb->width, height);
            if (face == k_no_handle)
                continue;
            window_set_state_image(thumb, state, face);
            image_release(face);
        }
        thumb->height = height;
    }
    auto* slider = reinterpret_cast<Slider*>(bar);
    slider_set_index(slider, slider_index(slider));
}

// ---- rows -------------------------------------------------------------------

/// Reports a click on a row or its title to its parent, as 3.1c's static controls do (the
/// toolkit's static class ignores it).
///
/// @param self the row or title clicked
/// @return true, the click handled
bool static_left_down(Window* self, const Message*) {
    window_notify_parent(self, NotifyCode::activated, 0);
    return true;
}

/// Makes a row its list's selected row and forwards a notice from its title to the list as the
/// row's own.
///
/// @param self the contents row
/// @param notify control notice
void row_on_notify(Window* self, const Notify* notify) {
    Window* list = self->parent;
    if (list == nullptr)
        return;
    reinterpret_cast<ContentsRow*>(self)->list->selected_item = self;
    Notify forwarded = *notify;
    forwarded.source = self;
    list->ops->on_notify(list, &forwarded);
}

WindowOps make_label_ops() {
    WindowOps ops = k_static_ops;
    ops.left_down = static_left_down;
    return ops;
}

ContentsRow* as_row(Window* w) {
    return reinterpret_cast<ContentsRow*>(w);
}

/// Shows the category's arrow: down while its headlines are shown, up otherwise.
///
/// The images are bound again on every pulse.
///
/// @param self the category row
void category_row_pulse(Window* self) {
    Window* icon = as_row(self)->icon;
    icon->ops->bind_images(
        icon,
        k_arrow_gaf,
        as_row(self)->expanded != 0 ? k_arrow_open : k_arrow_closed,
        0,
        screens::kWidgetKey
    );
}

/// Gives the selected headline's title the light small font.
///
/// @param self the headline row
void headline_row_select(Window* self) {
    ContentsRow* row = as_row(self);
    window_set_font_handle(row->label, row->fonts->small_light);
}

/// Gives any other headline's title the small font.
///
/// @param self the headline row
void headline_row_unselect(Window* self) {
    ContentsRow* row = as_row(self);
    window_set_font_handle(row->label, row->fonts->small);
}

constexpr screens::ListRowLook k_headline_look{headline_row_select, headline_row_unselect};

/// Gives the chosen category the light small font while the keyboard ring
/// shows. Until that, and after the ring hides, the title keeps the font it
/// was created with.
///
/// @param self the category row
void category_row_select(Window* self) {
    ContentsRow* row = as_row(self);
    if (row->label == nullptr || row->fonts == nullptr)
        return;
    const Desktop* desktop = services().desktop;
    if (desktop != nullptr && desktop->keyboard_ring != 0)
        window_set_font_handle(row->label, row->fonts->small_light);
    else if (row->label->font == row->fonts->small_light)
        window_set_font_handle(row->label, k_no_handle);
}

/// Clears a light title left on a category from when the ring was showing.
///
/// @param self the category row
void category_row_unselect(Window* self) {
    ContentsRow* row = as_row(self);
    if (row->label == nullptr || row->fonts == nullptr)
        return;
    if (row->label->font == row->fonts->small_light)
        window_set_font_handle(row->label, k_no_handle);
}

constexpr screens::ListRowLook k_category_look{category_row_select, category_row_unselect};

bool headline_row_draw(Window* self) {
    ContentsRow* row = as_row(self);
    return screens::list_row_draw(
        self, row->highlight != 0, row->list->selected_item, k_headline_look
    );
}

bool category_row_draw(Window* self) {
    ContentsRow* row = as_row(self);
    return screens::list_row_draw(
        self, row->highlight != 0, row->list->selected_item, k_category_look
    );
}

/// The text a program driving the game reads for this row.
///
/// @param self the row
/// @return `listed`
const char* row_listed(Window* self, int32_t /*state*/) {
    return as_row(self)->listed;
}

WindowOps make_row_ops() {
    WindowOps ops = make_label_ops();
    ops.destroy = console_window_free;
    ops.on_notify = row_on_notify;
    ops.draw = headline_row_draw;
    ops.state_text = row_listed;
    return ops;
}

WindowOps make_category_row_ops() {
    WindowOps ops = make_row_ops();
    ops.pulse = category_row_pulse;
    ops.draw = category_row_draw;
    return ops;
}

const WindowOps k_label_ops = make_label_ops();
const WindowOps k_row_ops = make_row_ops();
const WindowOps k_category_row_ops = make_category_row_ops();

/// Writes the text a program driving the game reads.
///
/// A category leads with "+ " while its headlines are hidden and "- " while
/// they are shown. A headline is the title alone.
///
/// @param[in,out] row the row
void refresh_listed(ContentsRow* row) {
    if (row->kind == RowKind::category) {
        const char* mark = row->expanded != 0 ? "- " : "+ ";
        std::snprintf(row->listed, sizeof row->listed, "%s%s", mark, row->text);
        return;
    }
    std::snprintf(row->listed, sizeof row->listed, "%s", row->text);
}

/// Makes the part of a row both row constructors share: the row across the list.
///
/// @param list the list box the row belongs to
/// @param kind whether the row is an entry or a category heading
/// @param text the row's label
/// @param fonts the fonts the row draws with
/// @param ops the window operations of the row kind
/// @return the new row
ContentsRow* row_create(
    screens::ByListBox* list,
    RowKind kind,
    const char* text,
    const screens::HatFonts* fonts,
    const WindowOps* ops
) {
    auto* row = as_row(console_window_alloc(sizeof(ContentsRow), ops));
    row->base.focusable = 0;
    row->kind = kind;
    row->list = list;
    row->fonts = fonts;
    row->reserved_after_list = k_row_reserved_after_list;
    row->row_height = k_row_height;
    row->highlight = 1;
    oa::boneyards::utf8::copy(row->text, k_row_text_bytes, text, 0);
    Window* list_window = &list->list.base;
    window_set_bounds(
        &row->base,
        list_window->x,
        list_window->y,
        screens::by_list_row_width(list_window),
        k_row_height,
        list_window,
        true
    );
    refresh_listed(row);
    return row;
}

/// Adds the row's title: 0xe in and ending at the row's right edge, so it lies inside the row and
/// moves with it.
///
/// @param[in,out] row the row
void row_add_label(ContentsRow* row) {
    Window* label = static_create();
    label->ops = &k_label_ops;
    window_set_bounds(
        label,
        row->base.x + k_title_dx,
        row->base.y,
        row->base.width - k_title_dx,
        k_row_height,
        &row->base,
        true
    );
    label->ops->set_caption(label, row->text, text_style::left);
    row->label = label;
}

/// Appends a category row: its name, the category record, whether its headlines are shown, and the
/// arrow showing that.
///
/// @param[in,out] list the ContentsList
/// @param name category name
/// @param category index of the category in the console's contents
/// @param fonts the Hat fonts
/// @return the row
ContentsRow* category_row_create(
    screens::ByListBox* list, const char* name, int32_t category, const screens::HatFonts* fonts
) {
    ContentsRow* row = row_create(list, RowKind::category, name, fonts, &k_category_row_ops);
    row->category = category;
    Window* icon = static_create();
    icon->ops = &k_label_ops;
    window_create(
        icon,
        k_category_icon_name,
        row->base.x,
        row->base.y + k_category_icon_dy,
        k_category_icon_size,
        k_category_icon_size,
        &row->base,
        true
    );
    row->icon = icon;
    row_add_label(row);
    list_box_add(&list->list, &row->base);
    return row;
}

/// Records whether a category row's headlines are shown.
///
/// @param[in,out] row category row
/// @param expanded 1 while its headlines are shown
void category_row_set_expanded(ContentsRow* row, uint8_t expanded) {
    row->expanded = expanded;
    refresh_listed(row);
}

/// Appends a headline row: its title and the locator of its document.
///
/// @param[in,out] list the ContentsList
/// @param title headline title
/// @param locator document the headline opens
/// @param fonts the Hat fonts
/// @return the row
ContentsRow* headline_row_create(
    screens::ByListBox* list, const char* title, const char* locator, const screens::HatFonts* fonts
) {
    ContentsRow* row = row_create(list, RowKind::headline, title, fonts, &k_row_ops);
    row_add_label(row);
    list_box_add(&list->list, &row->base);
    oa::boneyards::utf8::copy(row->locator, k_row_text_bytes, locator, 0);
    return row;
}

// ---- documents --------------------------------------------------------------

void contents_resolve(ContentsConsole* console, char* locator, bool rewrite);
bool contents_search_found(ContentsConsole* console, const char* key, const char* path);

/// Highlights the headline of the page in the view, when the list shows its category open; no
/// other headline is highlighted.
///
/// The headline becomes the list's selected row.
///
/// @param[in,out] console contents console
void contents_mark_page(ContentsConsole* console) {
    screens::ByListBox* list = console->list;
    list->selected_item = nullptr;
    for (int32_t i = 0; i < list->list.count; ++i) {
        ContentsRow* row = as_row(list_box_item(&list->list, i));
        if (row != nullptr && row->kind == RowKind::headline &&
            same_locator(row->locator, console->shown_locator)) {
            list->selected_item = &row->base;
            list->list.selected = i;
            return;
        }
    }
}

/// Opens a page: shows it in the view once it is read or has arrived, and highlights its headline.
///
/// @param[in,out] console contents console
/// @param locator the page as a headline or a link names it
void contents_open_page(ContentsConsole* console, const char* locator) {
    copy_locator(console->shown_locator, locator);
    copy_locator(console->page_locator, locator);
    contents_mark_page(console);
    contents_resolve(console, console->page_locator, true);
}

/// Refills the contents list: every category, with the headlines of the open ones.
///
/// The rows in view stay where they were, as far as the rows reach; the thumb is sized to the new
/// rows and the page's headline is highlighted.
///
/// @param[in,out] console contents console
/// @param select_first open the first category and load its first headline
void contents_rebuild(ContentsConsole* console, bool select_first) {
    screens::ByListBox* list = console->list;
    const screens::HatFonts* fonts = &console->owner->system.hat_fonts;
    const int32_t top = list->list.top;
    screens::by_list_box_clear(list);
    screens::ContentsRow
        rows[screens::kMaxContentsCategories * (screens::kMaxContentsHeadlines + 1)];
    char first[k_locator_bytes] = {};
    const std::size_t count = screens::contents_build_rows(
        &console->contents, select_first, rows, sizeof rows / sizeof rows[0], first, sizeof first
    );
    for (std::size_t i = 0; i < count; ++i) {
        screens::ContentsCategory& category = console->contents.categories[rows[i].category];
        if (rows[i].is_category) {
            ContentsRow* row = category_row_create(
                list, category.name, static_cast<int32_t>(rows[i].category), fonts
            );
            category_row_set_expanded(row, category.expanded ? 1 : 0);
            continue;
        }
        const screens::ContentsHeadline& headline = category.headlines[rows[i].headline];
        headline_row_create(list, headline.title, headline.locator, fonts);
        if (select_first && rows[i].category == 0 && rows[i].headline == 0)
            contents_open_page(console, first);
    }
    list_box_scroll(&list->list, top);
    contents_list_fit_thumb(&list->list);
    contents_mark_page(console);
}

bool keyboard_ring_shown() {
    const Desktop* desktop = services().desktop;
    return desktop != nullptr && desktop->keyboard_ring != 0;
}

/// Moves the highlight and reveals the row. It does not tell the parent, so
/// a category stays as it is and a headline stays closed.
void choose_contents_index(screens::ByListBox* box, int32_t index) {
    ListBox* list = &box->list;
    if (list->count <= 0)
        return;
    if (index < 0)
        index = 0;
    if (index >= list->count)
        index = list->count - 1;
    const int32_t old_top = list->top;
    int32_t top = list->top;
    if (index <= top)
        top = index;
    if (list->last <= index)
        top += index - list->last;
    if (top < 0)
        top = 0;
    list->selected = index;
    box->selected_item = list_box_item(list, index);
    if (top != old_top)
        list_box_scroll(list, top - old_top);
}

void reselect_category(ContentsConsole* console, int32_t category) {
    screens::ByListBox* box = console->list;
    for (int32_t i = 0; i < box->list.count; ++i) {
        ContentsRow* row = as_row(list_box_item(&box->list, i));
        if (row != nullptr && row->kind == RowKind::category && row->category == category) {
            choose_contents_index(box, i);
            return;
        }
    }
}

void set_category_expanded(ContentsConsole* console, int32_t category, bool expand) {
    console->contents.categories[category].expanded = expand;
    contents_rebuild(console, false);
    reselect_category(console, category);
}

/// Up and Down choose a row. Right opens a category, Left closes it, and
/// Return opens a headline or a closed category. While the ring is hidden
/// the list keeps the keys it has today.
bool contents_list_key_down(Window* self, const Message* msg) {
    if (msg == nullptr || msg->code != MessageCode::key_down)
        return false;
    if (!keyboard_ring_shown())
        return k_list_box_ops.key_down(self, msg);
    auto* box = reinterpret_cast<screens::ByListBox*>(self);
    ListBox* list = &box->list;
    const int32_t page = list->page > 0 ? list->page : 1;
    switch (msg->key) {
    case key::up:
        choose_contents_index(box, list->selected - 1);
        return true;
    case key::down:
        choose_contents_index(box, list->selected + 1);
        return true;
    case key::page_up:
        choose_contents_index(box, list->selected - page);
        return true;
    case key::page_down:
        choose_contents_index(box, list->selected + page);
        return true;
    case key::home:
        choose_contents_index(box, 0);
        return true;
    case key::end:
        choose_contents_index(box, list->count - 1);
        return true;
    case key::left:
    case key::right:
    case key::enter:
        break;
    default:
        return window_default_key_down(self, msg);
    }
    ContentsConsole* console = self->parent != nullptr ? as_console(self->parent) : nullptr;
    ContentsRow* row = as_row(list_box_item(list, list_box_selected(list)));
    if (row == nullptr || console == nullptr)
        return true;
    if (msg->key == key::right) {
        if (row->kind == RowKind::category && row->expanded == 0) {
            play_list_sound(console);
            set_category_expanded(console, row->category, true);
        }
        return true;
    }
    if (msg->key == key::left) {
        if (row->kind == RowKind::category && row->expanded != 0) {
            play_list_sound(console);
            set_category_expanded(console, row->category, false);
        }
        return true;
    }
    if (row->kind == RowKind::headline) {
        play_list_sound(console);
        contents_open_page(console, row->locator);
        return true;
    }
    if (row->kind == RowKind::category && row->expanded == 0) {
        play_list_sound(console);
        set_category_expanded(console, row->category, true);
    }
    return true;
}

/// Looks the searched name up in an arrived ladder index and fetches the page listing it.
///
/// The index is a run of words, a name followed by its line in the ladder;
/// names match without case. The page is the index locator with its letter
/// and ".byi" replaced by the page's number from 1 (a hundred lines to a
/// page) and ".htm". A name the index does not list opens a message box
/// saying so.
///
/// @param[in,out] console the Ladders console
/// @param key the index's locator
/// @param path file holding the index
/// @return true when the page is on its way
bool contents_search_found(ContentsConsole* console, const char* key, const char* path) {
    std::vector<uint8_t> bytes;
    int32_t line = -1;
    if (read_game_file(console->owner, path, &bytes))
        line = screens::ladder_index_position(
            reinterpret_cast<const char*>(bytes.data()), bytes.size(), console->search_name
        );
    if (line > -1) {
        char page[k_locator_bytes];
        screens::ladder_page_locator(key, line, page, sizeof page);
        contents_open_page(console, page);
        return true;
    }
    char message[k_locator_bytes + 0x40];
    screens::ladder_not_listed_message(console->search_name, message, sizeof message);
    message_box_create(message, message_box_buttons::ok, nullptr, nullptr, nullptr);
    return false;
}

/// Handles a document that has arrived, or failed to.
///
/// The contents file fills the list, the current page goes to the view, the
/// Ladders search's index finds the page listing the name, and anything
/// else is an image the view was waiting for. Then the next missing image
/// is fetched; the activity light stops once nothing is outstanding. A
/// downloaded temporary file is deleted.
///
/// @param[in,out] console contents console
/// @param key locator the document was asked for
/// @param path file holding the document
/// @param status fetch status; kFetchFailed for a failed fetch
void contents_file_arrived(
    ContentsConsole* console, const char* key, const char* path, int32_t status
) {
    bool idle = true;
    if (status != kFetchFailed) {
        if (std::strcmp(console->contents_locator, key) == 0) {
            read_contents(console, path);
            contents_rebuild(console, true);
        } else {
            if (std::strcmp(console->page_locator, key) == 0) {
                // The page is read whole, its bytes as stored.
                std::vector<uint8_t> bytes;
                if (read_game_file(console->owner, path, &bytes)) {
                    bytes.push_back(0);
                    html_view_set_text(console->view, reinterpret_cast<const char*>(bytes.data()));
                }
            } else if (std::strcmp(console->search_locator, key) == 0) {
                if (contents_search_found(console, key, path))
                    idle = false;
            } else {
                html_view_set_pair(console->view, key, path);
            }
            const char* pending = html_view_pop_pending(console->view);
            if (pending != nullptr) {
                idle = false;
                char locator[k_locator_bytes];
                copy_locator(locator, pending);
                contents_resolve(console, locator, false);
            }
        }
    }
    if (idle && console->frame->activity_light != nullptr)
        activity_light_stop(console->frame->activity_light);
    if (std::strstr(path, k_download_mark) != nullptr)
        std::remove(path);
}

/// Starts the activity light and resolves a locator: local files are handled at once, remote ones
/// are requested from the service.
///
/// @param[in,out] console contents console
/// @param[in,out] locator document locator (k_locator_bytes); rewritten to the resolved URL with
///     `rewrite`
/// @param rewrite whether the resolved URL replaces the locator text
void contents_resolve(ContentsConsole* console, char* locator, bool rewrite) {
    if (console->frame->activity_light != nullptr)
        activity_light_start(console->frame->activity_light);
    const screens::LocatorKind kind = screens::resolve_document_locator(
        &console->owner->config.service, console->server_type, locator, k_locator_bytes, rewrite
    );
    if (kind == screens::LocatorKind::local)
        contents_file_arrived(console, locator, locator, 0);
}

// ---- the console ------------------------------------------------------------

/// Builds the contents console when its bounds are set, then asks for its contents file.
///
/// Adds the "TOC" title, the ContentsList and the HTML view showing the
/// waiting page. The console drops its background colour and shows the
/// shared contents image, so the green console layer shows through the
/// image's keyed pixels and through the page, whose clear pixels leave what
/// is behind them.
///
/// @param self the contents console
void contents_build(Window* self) {
    ContentsConsole* console = as_console(self);
    drop_window_colour(self);
    window_set_state_image(self, k_image_state, console->owner->system.profile_image);
    Window* title = static_create();
    const Handle font = console->owner->system.hat_fonts.big;
    window_set_bounds(
        title, self->x, self->y + k_title_dy, k_title_width, font_line_height(font), self, true
    );
    title->ops->set_caption(title, "TOC", 0);
    window_set_font_handle(title, font);
    screens::ByListBox* list = screens::by_list_box_create(false, false);
    list->list.base.ops = &k_contents_list_ops;
    console->list = list;
    window_create(
        &list->list.base,
        "ContentsList",
        k_list_x,
        k_list_y,
        k_list_width,
        k_list_height,
        self,
        true
    );
    console->view = html_view_create(
        console->owner,
        self,
        k_view_dx,
        k_view_dy,
        self->width - k_view_width_inset,
        self->height - k_view_height_inset
    );
    // After a followed link the keyboard comes back to the list.
    console->view->keyboard_home = &list->list.base;
    html_view_set_text(console->view, k_waiting_text);
    console->view->base.visible = 1;
    contents_resolve(console, console->contents_locator, true);
    if (console->server_type != kContentServerLadders)
        return;
    const screens::HatFonts& fonts = console->owner->system.hat_fonts;
    screens::lobby_label_create(
        self,
        k_search_caption_x,
        k_search_caption_y,
        "Name",
        fonts.medium,
        k_search_caption_alignment,
        k_search_caption_anchor
    );
    console->search_entry = screens::lobby_edit_create(
        self,
        k_search_entry_x,
        k_search_entry_y,
        k_search_entry_width,
        fonts.small,
        k_search_entry_max,
        true
    );
    console->search_entry->scroll = 1;
    window_set_name(&console->search_entry->base, "Ladder search");
    console->search_button = button_create();
    screens::function_button_setup(
        console->search_button, false, k_search_button_x, k_search_button_y, "SEARCH", self
    );
    if (console->search_button->states.current != button_state::disabled)
        window_set_state(console->search_button, button_state::disabled);
}

/// Enables SEARCH on the shown Ladders console while the name entry holds text, and disables it
/// while the entry is empty.
///
/// @param self the contents console
void contents_pulse(Window* self) {
    if (!window_is_shown(self))
        return;
    ContentsConsole* console = as_console(self);
    if (console->server_type != kContentServerLadders || console->search_button == nullptr)
        return;
    Window* search = console->search_button;
    const bool disabled = search->states.current == button_state::disabled;
    if (console->search_entry->length != 0) {
        if (disabled)
            window_set_state(search, button_state::unpressed);
    } else if (!disabled) {
        window_set_state(search, button_state::disabled);
    }
}

/// Starts a Ladders search for the name in the entry, when it holds one.
///
/// The ladder's index for the name's first letter replaces the current
/// page's number and ".htm" with that letter and ".byi", and is fetched.
///
/// @param[in,out] console the Ladders console
void contents_search(ContentsConsole* console) {
    if (console->search_entry->length == 0)
        return;
    const char* name = single_edit_text(console->search_entry);
    screens::ladder_index_locator(
        console->page_locator, name[0], console->search_locator, k_locator_bytes
    );
    copy_locator(console->search_name, name);
    contents_resolve(console, console->search_locator, true);
}

/// Opens a clicked link or a selected headline, shows or hides a category's headlines, or runs the
/// Ladders search.
///
/// @param self the contents console
/// @param notify control notice
void contents_on_notify(Window* self, const Notify* notify) {
    ContentsConsole* console = as_console(self);
    if (notify->source == &console->view->base && notify->code == k_html_link_notify) {
        contents_open_page(console, console->view->link);
        return;
    }
    if (console->search_button != nullptr && notify->source == console->search_button) {
        contents_search(console);
        return;
    }
    if (notify->source != &console->list->list.base)
        return;
    play_list_sound(console);
    ListBox* list = &console->list->list;
    auto* row = reinterpret_cast<ContentsRow*>(list_box_item(list, list_box_selected(list)));
    if (row == nullptr)
        return;
    if (row->kind == RowKind::headline) {
        contents_open_page(console, row->locator);
        return;
    }
    if (row->kind != RowKind::category)
        return;
    const bool expand = row->expanded == 0;
    row->expanded = expand ? 1 : 0;
    console->contents.categories[row->category].expanded = expand;
    contents_rebuild(console, false);
}

// Documents stop arriving at a console that is gone.
void contents_destroy(Window* self) {
    MainFrame* frame = as_console(self)->frame;
    if (frame->contents_console == self)
        frame->contents_console = nullptr;
}

WindowOps make_console_ops() {
    WindowOps ops = console_panel_ops();
    ops.on_bounds = contents_build;
    ops.pulse = contents_pulse;
    ops.on_notify = contents_on_notify;
    ops.on_destroy = contents_destroy;
    return ops;
}

const WindowOps k_console_ops = make_console_ops();

} // namespace

void contents_console_open(Window* self) {
    ContentsConsole* console = as_console(self);
    auto* button = reinterpret_cast<ConsoleNavButton*>(console->console_button);
    window_set_state_image(&button->base, button_state::unpressed, button->lit);
    self->visible = 1;
    console->frame->contents_console = self;
}

void contents_console_file_arrived(
    Window* console, const char* url, const char* path, int32_t status
) {
    if (console != nullptr)
        contents_file_arrived(as_console(console), url, path, status);
    else if (std::strstr(path, k_download_mark) != nullptr)
        std::remove(path);
}

void contents_console_probe(const Window* console, MainFrameProbe* out) {
    const ContentsConsole* contents = reinterpret_cast<const ContentsConsole*>(console);
    out->contents_rows = contents->list->list.count;
    const html::Document& doc = contents->view->state->doc;
    const std::string text(doc.buffer.begin(), doc.buffer.end());
    out->contents_waiting = text.find("Waiting") != std::string::npos;
    copy_text(out->contents_page, sizeof out->contents_page, contents->page_locator);
    for (const html::Image& image : doc.images)
        out->contents_images_missing += image.loaded ? 0 : 1;
}

void console_screen_close(Window* console, Window* console_button) {
    auto* button = reinterpret_cast<ConsoleNavButton*>(console_button);
    window_set_state_image(&button->base, button_state::unpressed, button->normal);
    console->visible = 0;
}

void contents_console_close(Window* self) {
    console_screen_close(self, as_console(self)->console_button);
}

Window* contents_console_create(
    StatusWidget* owner,
    int32_t screen,
    Window* frame,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
) {
    const char* contents = k_help_contents;
    const char* name = "HelpConsole";
    ConsoleButton button = ConsoleButton::help;
    int32_t server_type = kContentServerNews;
    if (screen == console_screen::ladders) {
        contents = k_ladders_contents;
        name = "LadderConsole";
        button = ConsoleButton::ladders;
        server_type = kContentServerLadders;
    } else if (screen == console_screen::news) {
        contents = k_news_contents;
        name = "NewsConsole";
        button = ConsoleButton::news;
    }
    auto* console = reinterpret_cast<ContentsConsole*>(
        console_window_alloc(sizeof(ContentsConsole), &k_console_ops)
    );
    auto* main_frame = reinterpret_cast<MainFrame*>(frame);
    console->frame = main_frame;
    console->console_button = main_frame->console_buttons[static_cast<int32_t>(button)];
    console->server_type = server_type;
    console->owner = owner;
    copy_locator(console->contents_locator, contents);
    main_frame->contents_console = &console->base;
    window_create(&console->base, name, x, y, width, height, frame, true);
    return &console->base;
}

} // namespace oa::boneyards::console
