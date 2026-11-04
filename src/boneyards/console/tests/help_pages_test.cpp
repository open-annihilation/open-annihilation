// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every Help page of the BY1117a install, opened from the Help console's
// contents as a player opens it, or as a link to it opens it, and set in the
// bundled page faces. The loose pages in OA_BONEYARDS_HELP_DIR replace the
// expansion's pages of the same name. Each page loads with its images, draws
// text, and holds no word with a control character (a page's CR LF line ends
// are whitespace) or an &nbsp; left undecoded.
//
// With --frames DIR each page is written to DIR as two pictures: the console
// as the player first sees it (<page>-frame.png, 640x480) and the whole page
// on the console's green (<page>-page.png). With --pages DIR every .htm file
// in that folder is shown in the same view and written the same way, for
// pages that are not in the install (News and ladder pages).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "console_state.hpp"
#include "html_view.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/html.hpp"
#include "oa/boneyards/html_host_fonts.hpp"
#include "oa/boneyards/html_scripts.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/png.hpp"
#include "oa/platform/fonts.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

constexpr int32_t k_screen_width = 640;
constexpr int32_t k_screen_height = 480;
// The view's document is laid out this much narrower than the view.
constexpr int32_t k_document_inset = 0x17;
// Bytes below this are control characters.
constexpr unsigned char k_first_printable = 0x20;

// The install's archives and its palette.
struct Data {
    oa::AssetStore store{oa::boneyards_test::by_install()};
    oa::Palette palette{};
};

/// Reads a file of the install's archives for the status widget.
bool read_asset(void* user, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<Data*>(user)->store.read(path).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

/// Answers that the network and the session are up.
bool connected(void*) {
    return true;
}

/// Answers that the client activated.
ActivationResult activate(void*) {
    return ActivationResult::ok;
}

// The widget whose services a caption is read through.
StatusWidget* g_widget = nullptr;

/// Clicks the middle of a window with the left button.
///
/// @param widget status widget receiving the click
/// @param w window to click
void click_window(StatusWidget* widget, const Window* w) {
    const int32_t x = w->x + w->width / 2;
    const int32_t y = w->y + w->height / 2;
    Message down{};
    down.code = MessageCode::left_down;
    down.x = x;
    down.y = y;
    down.buttons = button_left;
    boneyards_status_widget_event(widget, down);
    Message up = down;
    up.code = MessageCode::left_up;
    up.buttons = 0;
    boneyards_status_widget_event(widget, up);
}

/// Returns a window's caption.
///
/// @param w window to read
/// @return the caption, empty when it has none
std::string caption(Window* w) {
    ScopedServices scope(g_widget);
    const char* text = w->ops->state_text(w, -1);
    return text != nullptr ? text : "";
}

/// Returns a contents row's title, the caption of its last child.
///
/// @param row row to read
/// @return the title, empty for a row without children
std::string row_title(Window* row) {
    if (row == nullptr || row->children.count == 0)
        return "";
    return caption(row->children.items[row->children.count - 1]);
}

/// Tests whether a contents row is a category: its first child is the arrow.
///
/// @param row row to test
/// @return true for a category row
bool is_category(Window* row) {
    return row != nullptr && row->children.count > 1 &&
           window_is_named(row->children.items[0], "CategoryIcon");
}

/// Finds the row of the contents list with a title.
///
/// @param list contents list
/// @param title title to look for
/// @return the row, or null
Window* row_titled(ListBox* list, const std::string& title) {
    for (int32_t i = 0; i < list->count; ++i)
        if (row_title(list_box_item(list, i)) == title)
            return list_box_item(list, i);
    return nullptr;
}

/// Lists the headline titles shown under a category row.
///
/// @param list contents list
/// @param category title of the category
/// @return the titles, empty while the category is closed
std::vector<std::string> headlines_under(ListBox* list, const std::string& category) {
    std::vector<std::string> titles;
    bool inside = false;
    for (int32_t i = 0; i < list->count; ++i) {
        Window* row = list_box_item(list, i);
        if (is_category(row)) {
            if (inside)
                break;
            inside = row_title(row) == category;
            continue;
        }
        if (inside)
            titles.push_back(row_title(row));
    }
    return titles;
}

/// Draws the status widget into a 640x480 picture.
///
/// @param widget status widget to draw
/// @return the picture's palette indices
std::vector<uint8_t> draw_frame(StatusWidget* widget) {
    std::vector<uint8_t> pixels(static_cast<std::size_t>(k_screen_width) * k_screen_height, 0);
    oa::Surface surface{};
    surface.width = k_screen_width;
    surface.height = k_screen_height;
    surface.pitch = k_screen_width;
    surface.pixels = pixels.data();
    surface.clip = oa::Rect32{0, 0, k_screen_width - 1, k_screen_height - 1};
    boneyards_status_widget_draw(widget, &surface);
    return pixels;
}

/// Writes palette indices as an indexed PNG.
///
/// @param path file to write
/// @param pixels rows of `width` indices
/// @param width picture width
/// @param height picture height
/// @param palette colours of the indices
void write_picture(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& pixels,
    int32_t width,
    int32_t height,
    const oa::Palette& palette
) {
    std::vector<oa::formats::png::Rgb> colors(static_cast<std::size_t>(OA_PALETTE_COLORS));
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i) {
        const oa::PaletteEntry& entry = palette.entries[i];
        colors[static_cast<std::size_t>(i)] = oa::formats::png::Rgb{entry.r, entry.g, entry.b};
    }
    oa::formats::png::Image image;
    image.header.width = static_cast<uint32_t>(width);
    image.header.height = static_cast<uint32_t>(height);
    image.header.bit_depth = 8;
    image.header.color_type = oa::formats::png::ColorType::palette;
    image.palette = colors;
    image.rows = pixels;
    std::vector<uint8_t> bytes;
    check(oa::formats::png::write(image, &bytes), "picture encoded: " + path.string());
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    check(out.good(), "picture written: " + path.string());
}

/// Finds the console's green behind the view: the frame drawn with the view
/// hidden, read at the view's middle.
///
/// @param widget status widget showing the console
/// @param view the console's HTML view
/// @return the palette entry there
oa::PaletteEntry console_green(StatusWidget* widget, HtmlView* view) {
    const uint8_t shown = view->base.visible;
    view->base.visible = 0;
    const std::vector<uint8_t> frame = draw_frame(widget);
    view->base.visible = shown;
    const int32_t x = view->base.x + view->base.width / 2;
    const int32_t y = view->base.y + view->base.height / 2;
    return widget->config.palette->entries[frame[static_cast<std::size_t>(y) * k_screen_width + x]];
}

/// Checks a shown page: it parsed, draws text, and no word holds a control
/// character or an undecoded &nbsp;.
///
/// @param view the view showing the page
/// @param name page name for the messages
void check_page(HtmlView* view, const std::string& name) {
    const html::Document& doc = view->state->doc;
    check(!doc.texts.empty(), name + ": the page has words");
    for (const html::TextRun& run : doc.texts) {
        const std::string_view text(run.text);
        const bool control = std::any_of(text.begin(), text.end(), [](char c) {
            return static_cast<unsigned char>(c) < k_first_printable;
        });
        check(!control, name + ": no word holds a control character");
        check(text.find("&nbsp") == std::string_view::npos, name + ": &nbsp; decoded");
    }
    const oa::Surface* canvas = html::document_surface(doc);
    check(canvas != nullptr, name + ": the page is painted");
    if (canvas == nullptr)
        return;
    int32_t inked = 0;
    for (int32_t y = 0; y < canvas->height; ++y)
        for (int32_t x = 0; x < canvas->width; ++x)
            inked += canvas->pixels[y * canvas->pitch + x] != html::k_canvas_clear_index ? 1 : 0;
    check(inked > 0, name + ": the page draws text");
}

// A sentence of a shipped page, which the expansion's page does not hold.
struct OverridePhrase {
    const char* stem{};
    const char* phrase{};
};

constexpr OverridePhrase k_override_phrases[] = {
    {"Help47", "Send feedback to Core Prime."},
    {"Help114", "Yard Dog insignia"},
    {"Help115", "Core Prime answers support questions"},
    {"Help226", "https://coreprime.net/register"},
    {"Help227", "Players gather at Core Prime."},
    {"Help48", "Each sector keeps a rank for Arm and a rank for Core."},
    {"Help53", "Core Contingency and Battle Tactics"},
    {"Help60", "There is no shop."},
    {"Help38", "Glicko-2"},
    {"Help45", "Change a password on the web."},
    {"Help75", "Forgot it? Reset it at coreprime.net/forgot"},
};

// Pages the 1998 contents left out, and pages that stay out of the contents.
constexpr const char* k_added_pages[] = {"Help66", "Help67", "Help112", "Help45"};
constexpr const char* k_left_out[] = {"Help39", "Help54", "Help168", "Help40", "Help68"};
constexpr std::size_t k_headline_count = 31;
constexpr std::size_t k_install_page_count = 36;

/// Joins the words the view is showing.
///
/// @param view the console's HTML view
/// @return the words, separated by spaces
std::string page_words(const HtmlView* view) {
    std::string text;
    for (const html::TextRun& run : view->state->doc.texts) {
        if (run.text == nullptr)
            continue;
        if (!text.empty())
            text.push_back(' ');
        text.append(run.text);
    }
    return text;
}

/// Checks that a shipped page is the one on screen.
///
/// @param view the console's HTML view
/// @param name the page's file name stem
void check_shipped_page(const HtmlView* view, const std::string& name) {
    for (const OverridePhrase& phrase : k_override_phrases) {
        if (name != phrase.stem)
            continue;
        const std::string words = page_words(view);
        check(words.find(phrase.phrase) != std::string::npos, name + ": the shipped page is shown");
        return;
    }
}

/// Reads a whole file. Empty when it cannot be opened.
///
/// @param path file to read
/// @return the bytes
std::vector<uint8_t> file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
    );
}

/// Reads a loose Help page for Rover's virtual-file reader.
///
/// @param user the override folder
/// @param path relative path Rover asked for
/// @param[out] out receives the page
/// @return true when the loose page was read
bool read_loose_override(void* user, const char* path, std::vector<uint8_t>* out) {
    if (user == nullptr || path == nullptr || out == nullptr)
        return false;
    const auto* folder = static_cast<const std::filesystem::path*>(user);
    std::string relative(path);
    for (char& c : relative)
        if (c == '\\')
            c = '/';
    const std::size_t slash = relative.find_last_of('/');
    const std::string name = slash == std::string::npos ? relative : relative.substr(slash + 1);
    const std::filesystem::path file = *folder / name;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

/// Proves a loose page wins over the expansion's page and leaves that file as it was.
///
/// @param install the BY1117a install, used as the expansion folder
/// @param overrides the folder of loose pages
void prove_override_wins(
    const std::filesystem::path& install, const std::filesystem::path& overrides
) {
    const std::filesystem::path original = install / "Boneyards" / "Help" / "Help47.htm";
    const std::vector<uint8_t> before = file_bytes(original);
    const std::vector<uint8_t> shipped = file_bytes(overrides / "Help47.htm");
    check(
        !before.empty() && !shipped.empty() && before != shipped,
        "the shipped page differs from the expansion's page"
    );
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / "G24-help-override";
    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    std::filesystem::create_directories(scratch / "player", error);
    std::filesystem::create_directories(scratch / "game", error);
    rover_session_configure((scratch / "game").string().c_str(), "");
    rover_session_set_player_folder((scratch / "player").string().c_str());
    rover_session_set_expansion_folder(install.string().c_str());
    std::filesystem::path override_folder = overrides;
    rover_session_set_virtual_files(read_loose_override, &override_folder);
    std::vector<uint8_t> got;
    check(rover_session_read_file("./boneyards/Help/Help47.htm", &got), "the loose page is read");
    check(got == shipped, "the loose page wins over the expansion's page");
    check(file_bytes(original) == before, "the expansion's page is unchanged");
    std::filesystem::remove_all(scratch, error);
}

/// Writes the shown page as the console frame and as the whole page.
///
/// @param folder folder receiving the pictures; empty writes none
/// @param name file name stem
/// @param widget status widget showing the console
/// @param view the console's HTML view
/// @param green the console's green behind the page
void write_page(
    const std::filesystem::path& folder,
    const std::string& name,
    StatusWidget* widget,
    HtmlView* view,
    const oa::PaletteEntry& green
) {
    if (folder.empty())
        return;
    write_picture(
        folder / (name + "-frame.png"),
        draw_frame(widget),
        k_screen_width,
        k_screen_height,
        *widget->config.palette
    );
    const html::Document& doc = view->state->doc;
    const oa::Surface* canvas = html::document_surface(doc);
    if (canvas == nullptr)
        return;
    std::vector<uint8_t> rows;
    rows.reserve(static_cast<std::size_t>(canvas->width) * canvas->height);
    for (int32_t y = 0; y < canvas->height; ++y)
        for (int32_t x = 0; x < canvas->width; ++x)
            rows.push_back(canvas->pixels[y * canvas->pitch + x]);
    oa::Palette palette = doc.env.palette;
    palette.entries[html::k_canvas_clear_index] = green;
    write_picture(folder / (name + "-page.png"), rows, canvas->width, canvas->height, palette);
}

/// Returns a page's file name without its folder or extension.
///
/// @param locator the page's locator
/// @return the stem
std::string page_stem(const char* locator) {
    std::string text(locator);
    const std::size_t slash = text.find_last_of("/\\");
    if (slash != std::string::npos)
        text.erase(0, slash + 1);
    const std::size_t dot = text.rfind('.');
    if (dot != std::string::npos)
        text.erase(dot);
    return text;
}

/// Checks the page the console shows and writes it with --frames.
///
/// @param widget status widget showing the Help console
/// @param console the Help console
/// @param view the console's HTML view
/// @param frames folder for pictures; empty writes none
/// @param green the console's green behind the page
/// @return the page's file name stem
std::string take_page(
    StatusWidget* widget,
    Window* console,
    HtmlView* view,
    const std::filesystem::path& frames,
    const oa::PaletteEntry& green
) {
    MainFrameProbe probe{};
    contents_console_probe(console, &probe);
    const std::string name = page_stem(probe.contents_page);
    check(probe.contents_images_missing == 0, name + ": every image of the page is loaded");
    check_page(view, name);
    check_shipped_page(view, name);
    write_page(frames, name, widget, view, green);
    return name;
}

/// Opens every headline of every category of the Help console, checks each
/// page and writes it with --frames.
///
/// @param widget status widget showing the Help console
/// @param console the Help console
/// @param view the console's HTML view
/// @param frames folder for pictures; empty writes none
/// @param[out] opened stems of the pages opened
void open_every_headline(
    StatusWidget* widget,
    Window* console,
    HtmlView* view,
    const std::filesystem::path& frames,
    std::vector<std::string>* opened
) {
    auto* list = reinterpret_cast<ListBox*>(window_find_named(console, "ContentsList", nullptr));
    check(list != nullptr, "contents list");
    if (list == nullptr)
        return;
    std::vector<std::string> categories;
    for (int32_t i = 0; i < list->count; ++i)
        if (is_category(list_box_item(list, i)))
            categories.push_back(row_title(list_box_item(list, i)));
    check(categories.size() == 6, "Help.def has six categories");
    const oa::PaletteEntry green = console_green(widget, view);
    for (const std::string& category : categories) {
        // A closed category opens on a click; the first one starts open.
        if (headlines_under(list, category).empty())
            click_window(widget, row_titled(list, category));
        const std::vector<std::string> titles = headlines_under(list, category);
        check(!titles.empty(), category + ": headlines shown");
        for (const std::string& title : titles) {
            Window* row = row_titled(list, title);
            check(row != nullptr, title + ": headline row");
            if (row == nullptr)
                continue;
            click_window(widget, row);
            opened->push_back(take_page(widget, console, view, frames, green));
        }
        click_window(widget, row_titled(list, category));
    }
}

/// Opens the install's Help pages that no headline names, as a link to one
/// opens it: through the view's link notice to the console.
///
/// @param root the install
/// @param widget status widget showing the Help console
/// @param console the Help console
/// @param view the console's HTML view
/// @param frames folder for pictures; empty writes none
/// @param[in,out] opened stems of the pages opened; grows by the pages opened here
void open_linked_pages(
    const std::filesystem::path& root,
    StatusWidget* widget,
    Window* console,
    HtmlView* view,
    const std::filesystem::path& frames,
    std::vector<std::string>* opened
) {
    std::vector<std::string> files;
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(root / "Boneyards" / "Help", error))
        if (entry.path().extension() == ".htm")
            files.push_back(entry.path().filename().string());
    std::sort(files.begin(), files.end());
    const oa::PaletteEntry green = console_green(widget, view);
    for (const std::string& file : files) {
        const std::string stem = std::filesystem::path(file).stem().string();
        if (std::find(opened->begin(), opened->end(), stem) != opened->end())
            continue;
        const std::string locator = "Boneyards/Help/" + file;
        view->link = locator.c_str();
        Notify notify{};
        notify.code = k_html_link_notify;
        notify.source = &view->base;
        {
            ScopedServices scope(widget);
            console->ops->on_notify(console, &notify);
        }
        view->link = nullptr;
        opened->push_back(take_page(widget, console, view, frames, green));
    }
}

/// Shows every .htm file of a folder in the view and writes it.
///
/// @param folder folder holding the pages
/// @param widget status widget showing the Help console
/// @param view the console's HTML view
/// @param frames folder for pictures
void show_folder_pages(
    const std::filesystem::path& folder,
    StatusWidget* widget,
    HtmlView* view,
    const std::filesystem::path& frames
) {
    std::vector<std::filesystem::path> pages;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        if (entry.path().extension() == ".htm")
            pages.push_back(entry.path());
    std::sort(pages.begin(), pages.end());
    const oa::PaletteEntry green = console_green(widget, view);
    for (const std::filesystem::path& page : pages) {
        std::ifstream in(page, std::ios::binary);
        const std::string text(
            (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()
        );
        {
            ScopedServices scope(widget);
            html_view_set_text(view, text.c_str());
        }
        const std::string name = page.stem().string();
        check_page(view, name);
        write_page(frames, name, widget, view, green);
    }
}

/// Compares two names without regard to ASCII case.
bool same_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        unsigned char left = static_cast<unsigned char>(a[i]);
        unsigned char right = static_cast<unsigned char>(b[i]);
        if (left >= 'A' && left <= 'Z')
            left = static_cast<unsigned char>(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z')
            right = static_cast<unsigned char>(right - 'A' + 'a');
        if (left != right)
            return false;
    }
    return true;
}

/// The next path component of `path`, starting at `at`. `at` advances past it.
std::string next_component(const std::string& path, std::size_t* at) {
    while (*at < path.size() && (path[*at] == '/' || path[*at] == '\\'))
        ++*at;
    const std::size_t start = *at;
    while (*at < path.size() && path[*at] != '/' && path[*at] != '\\')
        ++*at;
    return path.substr(start, *at - start);
}

/// A directory entry named exactly `part`, or nullopt.
std::filesystem::path exact_child(const std::filesystem::path& directory, const std::string& part) {
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), last; !error && it != last;
         it.increment(error)) {
        if (it->path().filename().string() == part)
            return it->path();
    }
    return {};
}

/// A directory entry named `part` in any ASCII case, or nullopt.
std::filesystem::path
folded_child(const std::filesystem::path& directory, const std::string& part) {
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), last; !error && it != last;
         it.increment(error)) {
        if (same_ascii(it->path().filename().string(), part))
            return it->path();
    }
    return {};
}

/// Reads `path` only when every component matches the directory listing exactly.
///
/// A backslash is a separator here, and `Images` does not open `images`. The
/// volume's own case rules are not consulted, so the check holds on a
/// case-insensitive disk.
bool read_exact(void* user, const char* path, std::vector<uint8_t>* out) {
    if (path == nullptr || out == nullptr)
        return false;
    const auto* root = static_cast<const std::filesystem::path*>(user);
    std::filesystem::path current = *root;
    const std::string text(path);
    std::size_t at = 0;
    while (at < text.size()) {
        const std::string part = next_component(text, &at);
        if (part.empty() || part == ".")
            continue;
        current = exact_child(current, part);
        if (current.empty())
            return false;
    }
    std::ifstream in(current, std::ios::binary);
    if (!in)
        return false;
    *out =
        std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return static_cast<bool>(in) || !out->empty();
}

/// The picture sources a page names.
std::vector<std::string> picture_sources(const std::string& page) {
    std::vector<std::string> sources;
    const std::string marker = "SRC=\"";
    for (std::size_t at = 0; (at = page.find(marker, at)) != std::string::npos;) {
        at += marker.size();
        const std::size_t end = page.find('"', at);
        if (end == std::string::npos)
            break;
        sources.push_back(page.substr(at, end - at));
        at = end + 1;
    }
    return sources;
}

/// Copies one picture to `dest_root`, keeping the spelling the disk stored.
bool copy_picture(
    const std::filesystem::path& install,
    const std::filesystem::path& dest_root,
    const std::string& source
) {
    std::filesystem::path from = install;
    const std::string text(source);
    std::size_t at = 0;
    while (at < text.size()) {
        const std::string part = next_component(text, &at);
        if (part.empty() || part == ".")
            continue;
        from = folded_child(from, part);
        if (from.empty())
            return false;
    }
    const std::filesystem::path relative = from.lexically_relative(install);
    const std::filesystem::path dest = dest_root / relative;
    std::error_code error;
    std::filesystem::create_directories(dest.parent_path(), error);
    std::filesystem::copy_file(
        from, dest, std::filesystem::copy_options::overwrite_existing, error
    );
    return !error;
}

/// Loads every Help page from a folder whose picture names differ in case
/// and separators from the names the pages use.
///
/// The widget's reader accepts only the spelling on disk. The page reader
/// then asks the session, which matches each component without regard to case.
void load_pictures_case_sensitive(
    StatusWidget* widget, HtmlView* view, const std::filesystem::path& install
) {
    std::vector<std::filesystem::path> pages;
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(install / "Boneyards" / "Help", error))
        if (entry.path().extension() == ".htm")
            pages.push_back(entry.path());
    std::sort(pages.begin(), pages.end());
    check(!pages.empty(), "Help pages to copy");
    if (pages.empty())
        return;

    std::random_device seed;
    const std::filesystem::path dest =
        std::filesystem::temp_directory_path() / ("G23-help-pictures-" + std::to_string(seed()));
    std::filesystem::create_directories(dest, error);
    check(!error, "case-sensitive picture folder");
    if (error)
        return;

    std::vector<std::string> copied;
    for (const std::filesystem::path& page : pages) {
        std::ifstream in(page, std::ios::binary);
        const std::string text(
            (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()
        );
        for (const std::string& source : picture_sources(text)) {
            if (std::find(copied.begin(), copied.end(), source) != copied.end())
                continue;
            check(copy_picture(install, dest, source), "copy " + source);
            copied.push_back(source);
        }
    }
    check(!copied.empty(), "pages name pictures");

    const FileReader saved = widget->config.files;
    widget->config.files = FileReader{const_cast<std::filesystem::path*>(&dest), read_exact};
    rover_session_configure(dest.string().c_str(), "");
    for (const std::filesystem::path& page : pages) {
        std::ifstream in(page, std::ios::binary);
        const std::string text(
            (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()
        );
        {
            ScopedServices scope(widget);
            html_view_set_text(view, text.c_str());
        }
        std::vector<const char*> pending;
        html::document_pending_images(view->state->doc, &pending);
        check(pending.empty(), page.filename().string() + ": every picture loads");
    }
    widget->config.files = saved;
    rover_session_configure(install.string().c_str(), "");
    std::filesystem::remove_all(dest, error);
}

/// Opens the bundled page faces and the chain behind them.
///
/// @param[out] chain the chain for characters the faces lack
/// @return the face library, or null
oa::platform::FontLibrary* open_page_faces(html::FallbackFonts** chain) {
    const std::string dir = OA_TEXT_FONTS_DIR;
    const std::string dejavu = dir + "/DejaVuSans.ttf";
    const std::string bold = dir + "/DejaVuSans-Bold.ttf";
    const std::string emoji = dir + "/NotoEmoji.ttf";
    *chain = html::fallback_fonts_open_files(
        html::FallbackFiles{dejavu.c_str(), bold.c_str(), nullptr, emoji.c_str()}
    );
    html::page_script_fonts_use(*chain);
    char error[512]{};
    oa::platform::FontLibrary* library =
        oa::platform::font_library_open(OA_FONTS_DIR, error, static_cast<int32_t>(sizeof error));
    check(library != nullptr, error[0] != '\0' ? error : "the page faces open");
    return library;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path frames;
    std::filesystem::path pages;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0)
            frames = argv[i + 1];
        else if (std::strcmp(argv[i], "--pages") == 0)
            pages = argv[i + 1];
    }
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the Help pages", {"Boneyards/Help/Help.def"});
    const std::filesystem::path overrides = OA_BONEYARDS_HELP_DIR;
    boneyards_console_set_help_override_directory(overrides.string().c_str());
    prove_override_wins(root, overrides);
    const std::filesystem::path expansion_help = root / "Boneyards" / "Help";
    std::vector<std::pair<std::string, std::vector<uint8_t>>> expansion_before;
    for (const auto& entry : std::filesystem::directory_iterator(expansion_help)) {
        if (!entry.is_regular_file())
            continue;
        expansion_before.emplace_back(entry.path().filename().string(), file_bytes(entry.path()));
    }
    Data data;
    data.store.mount(root / "by.ccx");
    data.store.mount(root / "totala1.hpi");
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int i = 0; i < OA_PALETTE_COLORS; ++i)
        data.palette.entries[i] = oa::PaletteEntry{raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};

    html::FallbackFonts* chain = nullptr;
    oa::platform::FontLibrary* library = open_page_faces(&chain);
    if (library == nullptr)
        return 1;

    StatusWidgetConfig config{};
    config.files = FileReader{&data, read_asset};
    config.palette = &data.palette;
    config.text_fonts = html::host_font_service(library);
    config.service.network_available = connected;
    config.service.activate = activate;
    config.service.connected = connected;
    StatusWidget* widget = boneyards_status_widget_create(config);
    g_widget = widget;
    check(widget != nullptr, "status widget");
    MainFrame* frame = widget != nullptr ? widget->system.main_frame : nullptr;
    if (widget != nullptr) {
        click_window(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
        frame = widget->system.main_frame;
    }
    check(frame != nullptr, "Main Frame shown");
    if (frame != nullptr) {
        for (int32_t i = 0; frame->console_screen != console_screen::login && i < 160; ++i)
            boneyards_status_widget_tick(widget);
        {
            ScopedServices scope(widget);
            main_frame_enable_buttons(frame);
        }
        click_window(widget, frame->console_buttons[static_cast<int32_t>(ConsoleButton::help)]);
        Window* console = frame->active_screen;
        auto* view =
            console != nullptr
                ? reinterpret_cast<HtmlView*>(window_find_named(console, "HTMLView", nullptr))
                : nullptr;
        check(view != nullptr, "Help console and its view");
        if (view != nullptr) {
            std::printf(
                "Help pages laid out %d pixels wide\n", view->base.width - k_document_inset
            );
            std::vector<std::string> opened;
            open_every_headline(widget, console, view, frames, &opened);
            check(opened.size() == k_headline_count, "every headline of Help.def opens its page");
            for (const char* added : k_added_pages)
                check(
                    std::find(opened.begin(), opened.end(), added) != opened.end(),
                    std::string(added) + " is reachable from the contents"
                );
            for (const char* left_out : k_left_out)
                check(
                    std::find(opened.begin(), opened.end(), left_out) == opened.end(),
                    std::string(left_out) + " stays out of the contents"
                );
            auto* list =
                reinterpret_cast<ListBox*>(window_find_named(console, "ContentsList", nullptr));
            if (list != nullptr) {
                if (headlines_under(list, "Getting Started").empty())
                    click_window(widget, row_titled(list, "Getting Started"));
                check(
                    row_titled(list, "Where to begin") != nullptr, "the shipped contents are shown"
                );
                check(
                    row_titled(list, "Editing a profile") != nullptr,
                    "editing a profile is reachable"
                );
            }
            open_linked_pages(root, widget, console, view, frames, &opened);
            check(opened.size() == k_install_page_count, "every Help page of the install opens");
            for (const auto& [filename, bytes] : expansion_before)
                check(
                    file_bytes(expansion_help / filename) == bytes,
                    filename + " in the expansion is unchanged"
                );
            std::printf("%zu Help pages opened\n", opened.size());
            if (!pages.empty())
                show_folder_pages(pages, widget, view, frames.empty() ? pages : frames);
            load_pictures_case_sensitive(widget, view, root);
        }
    }
    if (widget != nullptr)
        boneyards_status_widget_destroy(widget);
    oa::platform::font_library_close(library);
    html::page_script_fonts_use(nullptr);
    html::fallback_fonts_close(chain);
    if (g_failures != 0) {
        std::fprintf(stderr, "help pages: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("help pages: ok");
    return 0;
}
