// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The lobby banner strip: ads.def, the ad server's pictures, and the local sample frames.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "ad_banner.hpp"
#include "console_state.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/registration.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/png.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct Data {
    oa::AssetStore store{oa::boneyards_test::by_install()};
};

bool read_asset(void* user, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<Data*>(user)->store.read(path).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

struct Store {
    std::map<std::string, std::vector<uint8_t>> files;
    std::map<std::string, int> reads;
};

bool read_store(void* context, const char* path, std::vector<uint8_t>* out) {
    auto* store = static_cast<Store*>(context);
    if (store == nullptr || path == nullptr || out == nullptr)
        return false;
    store->reads[path] += 1;
    const auto found = store->files.find(path);
    if (found == store->files.end())
        return false;
    *out = found->second;
    return true;
}

int g_fetches = 0;
char g_fetch_url[640]{};

ServiceResult fetch_failed(void*, const char* url, char* dest, std::size_t capacity) {
    ++g_fetches;
    g_fetch_url[0] = '\0';
    if (url != nullptr)
        std::snprintf(g_fetch_url, sizeof g_fetch_url, "%s", url);
    if (dest != nullptr && capacity > 0)
        dest[0] = '\0';
    return ServiceResult::failed;
}

bool service_network(void*) {
    return true;
}

ActivationResult service_activate(void*) {
    return ActivationResult::ok;
}

bool service_connected(void*) {
    return true;
}

void service_events(void*, const ServiceEvents*) {
}

void service_pump(void*) {
}

Window* create_screen(void*, int32_t, Window* frame, int32_t x, int32_t y, int32_t w, int32_t h) {
    Window* window = window_create_plain();
    window_create(window, "TestScreen", x, y, w, h, frame, true);
    return window;
}

Message pointer(MessageCode code, int32_t x, int32_t y, uint32_t buttons) {
    Message msg{};
    msg.code = code;
    msg.x = x;
    msg.y = y;
    msg.buttons = buttons;
    return msg;
}

std::string g_opened;
int g_open_count = 0;
std::vector<std::string> g_ad_events;

void note_ad_event(const char* wire, void*) {
    g_ad_events.emplace_back(wire != nullptr ? wire : "");
}

std::string ad_event_text(int32_t banner_id, int32_t kind) {
    char text[160];
    std::snprintf(
        text,
        sizeof text,
        "{[Rover<I:banner=%d><S:command=ad_event><S:game=ta><I:kind=%d><S:lang=en>]}",
        banner_id,
        kind
    );
    return text;
}

bool browser_available() {
    return true;
}

bool browser_open(const char* address) {
    ++g_open_count;
    g_opened = address != nullptr ? address : "";
    return true;
}

bool parse_one(const char* text, ParsedBanner* banner) {
    int32_t count = 0;
    if (!parse_ads_def(text, banner, 1, &count) || count != 1)
        return false;
    return true;
}

void test_parser() {
    ParsedBanner banner{};
    const char* golden =
        "{[Ads<S:command=banner><I:id=1><S:file=/boneyards/ta/ads/cavedog.png><I:seconds=30>"
        "<I:weight=2><S:link=https%6//example.com/a%7b%5c>]}";
    check(parse_one(golden, &banner), "the admin console's banner line parses");
    check(
        banner.id == 1 && std::strcmp(banner.file, "/boneyards/ta/ads/cavedog.png") == 0 &&
            banner.seconds == 30 && banner.pulses == 900 && banner.weight == 2 &&
            std::strcmp(banner.link, "https://example.com/a=b>c") == 0,
        "id, file, thirty seconds, weight and the escaped link"
    );

    check(parse_one("{[Ads<S:command=banner><S:file=/a.png>]}", &banner), "seconds may be omitted");
    check(
        banner.seconds == 0 && banner.pulses == k_ad_banner_period && banner.weight == 1,
        "a missing interval is 900 pulses and a missing weight is 1"
    );
    check(
        parse_one("{[Ads<S:command=banner><S:file=/a.png><I:seconds=1><I:weight=0>]}", &banner) &&
            banner.pulses == 30 && banner.weight == 0,
        "one second is 30 pulses"
    );
    check(
        banner_interval_pulses(0) == 900 && banner_interval_pulses(-3) == 900,
        "below one second stays at 900"
    );
    check(
        banner_interval_pulses(200000000) == INT32_MAX, "an interval that does not fit is capped"
    );

    check(
        parse_one(
            "{[Ads<S:command=banner><S:file=/a.png><S:note=hello><I:seconds=2><I:weight=3>]}",
            &banner
        ) && banner.pulses == 60 &&
            banner.weight == 3,
        "an unknown field is ignored"
    );
    check(
        parse_one("{[Ads<S:command=banner><S:file=/a.png><S:link=a%9b>]}", &banner) &&
            banner.link[0] == 'a' && static_cast<unsigned char>(banner.link[1]) == 0xff &&
            banner.link[2] == 'b',
        "a percent digit outside 0 to 8 is the byte ff"
    );

    int32_t count = 0;
    ParsedBanner rows[2]{};
    check(
        !parse_ads_def(
            "{[Ads<S:command=banner><S:file=/a.png>]}{[Ads<S:command=nope><S:file=/b.png>]}",
            rows,
            2,
            &count
        ),
        "one bad record rejects the file"
    );
    check(
        !parse_ads_def("{[Ads<S:command=banner><S:file=/a.png><S:file=/b.png>]}", rows, 2, &count),
        "a duplicate key rejects the file"
    );
    check(
        !parse_ads_def("{[Ads<S:command=banner><S:file=/a.png><S:link=ab%>]}", rows, 2, &count),
        "a trailing percent rejects the file"
    );
    check(parse_ads_def(" \n\t", rows, 2, &count) && count == 0, "a blank file is an empty list");

    std::string many;
    for (int32_t i = 0; i < 65; ++i)
        many += "{[Ads<S:command=banner><I:id=" + std::to_string(i) + "><S:file=/a.png>]}";
    ParsedBanner full[64]{};
    check(!parse_ads_def(many, full, 64, &count), "a sixty-fifth banner rejects the file");
    many.resize(many.rfind("{[Ads"));
    check(parse_ads_def(many, full, 64, &count) && count == 64, "sixty banners are a list");

    const int32_t weights[] = {2, 1, 0};
    int32_t order[8]{};
    int32_t order_count = 0;
    expand_banner_order(weights, 2, order, 8, &order_count);
    check(
        order_count == 3 && order[0] == 0 && order[1] == 1 && order[2] == 0,
        "weights 2 and 1 show the first, the second, then the first"
    );
    expand_banner_order(weights + 1, 2, order, 8, &order_count);
    check(
        order_count == 2 && order[0] == 0 && order[1] == 1, "a weight below 1 counts as one showing"
    );
}

void test_urls() {
    char url[640];
    check(
        banner_list_url("http://ads.example/banner", "id=1", nullptr, url, sizeof url) &&
            std::strcmp(url, "http://ads.example:80/boneyards/ta/ads/ads.def?id=1") == 0,
        "the catalogue is that host's ads.def with the params"
    );
    check(
        banner_list_url("https://ads.example:8080/ignored", nullptr, nullptr, url, sizeof url) &&
            std::strcmp(url, "http://ads.example:8080/boneyards/ta/ads/ads.def") == 0,
        "https keeps the port and is fetched as http"
    );
    check(
        banner_list_url("http://ads.example", "?id=1", nullptr, url, sizeof url) &&
            std::strcmp(url, "http://ads.example:80/boneyards/ta/ads/ads.def?id=1") == 0,
        "params that already start with a question mark are appended"
    );
    check(
        banner_list_url("http://ads.example", "&id=1", nullptr, url, sizeof url) &&
            std::strcmp(url, "http://ads.example:80/boneyards/ta/ads/ads.def&id=1") == 0,
        "params that start with an ampersand are appended"
    );
    check(
        banner_list_url("http://ads.example/banner?cluster=alpha", "id=1", "fr", url, sizeof url) &&
            std::strcmp(
                url, "http://ads.example:80/boneyards/ta/ads/ads.def?cluster=alpha&id=1&lang=fr"
            ) == 0,
        "the catalogue keeps the ad server's query and adds the language"
    );
    check(
        banner_list_url("http://ads.example/banner", nullptr, "en", url, sizeof url) &&
            std::strcmp(url, "http://ads.example:80/boneyards/ta/ads/ads.def?lang=en") == 0,
        "a language with no other query is the only parameter"
    );
    check(
        banner_list_url(
            "http://ads.example/banner?cluster=alpha#top", nullptr, "de", url, sizeof url
        ) &&
            std::strcmp(
                url, "http://ads.example:80/boneyards/ta/ads/ads.def?cluster=alpha&lang=de"
            ) == 0,
        "a fragment on the ad server url is not part of the query"
    );
    check(
        !banner_list_url("http://", nullptr, nullptr, url, sizeof url), "an empty host is refused"
    );
    check(
        !banner_list_url("http://ads.example:0/", nullptr, nullptr, url, sizeof url),
        "port 0 is refused"
    );
    check(
        !banner_list_url("http://ads.example:99999/", nullptr, nullptr, url, sizeof url),
        "a port above 65535 is refused"
    );
    check(
        banner_file_url(
            "https://ads.example:8080/ignored", "/boneyards/ta/ads/a.png", url, sizeof url
        ) && std::strcmp(url, "http://ads.example:8080/boneyards/ta/ads/a.png") == 0,
        "a banner file is the same host and carries no params"
    );
}

std::vector<oa::formats::png::Rgb> png_colours(const oa::Palette& lobby) {
    std::vector<oa::formats::png::Rgb> colors(static_cast<std::size_t>(OA_PALETTE_COLORS));
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i) {
        const oa::PaletteEntry& entry = lobby.entries[i];
        colors[static_cast<std::size_t>(i)] = oa::formats::png::Rgb{entry.r, entry.g, entry.b};
    }
    return colors;
}

std::vector<uint8_t> indexed_png(uint8_t index, std::vector<oa::formats::png::Rgb> colors) {
    oa::formats::png::Header header{};
    header.width = static_cast<uint32_t>(k_ad_banner_width);
    header.height = static_cast<uint32_t>(k_ad_banner_height);
    header.bit_depth = 8;
    header.color_type = oa::formats::png::ColorType::palette;
    std::vector<uint8_t> rows(
        static_cast<std::size_t>(k_ad_banner_width) * static_cast<std::size_t>(k_ad_banner_height),
        index
    );
    oa::formats::png::Image image;
    image.header = header;
    image.palette = colors;
    image.rows = rows;
    std::vector<uint8_t> out;
    check(oa::formats::png::write(image, &out), "indexed banner written");
    return out;
}

void test_png_map() {
    oa::Palette palette{};
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i)
        palette.entries[i] = oa::PaletteEntry{1, 1, 1, 0};
    palette.entries[4] = oa::PaletteEntry{255, 0, 0, 0};
    palette.entries[5] = oa::PaletteEntry{0, 0, 255, 0};
    palette.entries[7] = oa::PaletteEntry{20, 30, 40, 0};
    palette.entries[9] = oa::PaletteEntry{10, 10, 10, 0};

    std::vector<uint8_t> indices(
        static_cast<std::size_t>(k_ad_banner_width) * static_cast<std::size_t>(k_ad_banner_height)
    );
    std::vector<oa::formats::png::Rgb> foreign(
        static_cast<std::size_t>(OA_PALETTE_COLORS), oa::formats::png::Rgb{9, 9, 9}
    );
    foreign[7] = oa::formats::png::Rgb{255, 0, 0};
    const std::vector<uint8_t> foreign_file = indexed_png(7, foreign);
    check(
        !banner_map_png(foreign_file, nullptr, indices.data()),
        "an indexed picture needs the lobby palette"
    );
    check(
        banner_map_png(foreign_file, &palette, indices.data()) && indices[0] == 4 &&
            indices.back() == 4,
        "an indexed picture maps each pixel to the nearest lobby colour"
    );
    const std::vector<uint8_t> same = indexed_png(7, png_colours(palette));
    check(
        banner_map_png(same, &palette, indices.data()) && indices[0] == 7 && indices.back() == 7,
        "a picture in the lobby palette keeps its colours"
    );

    oa::formats::png::Header grey{};
    grey.width = static_cast<uint32_t>(k_ad_banner_width);
    grey.height = static_cast<uint32_t>(k_ad_banner_height);
    grey.bit_depth = 8;
    grey.color_type = oa::formats::png::ColorType::grey;
    std::vector<uint8_t> grey_rows(indices.size(), 10);
    oa::formats::png::Image grey_image;
    grey_image.header = grey;
    grey_image.rows = grey_rows;
    std::vector<uint8_t> grey_file;
    check(oa::formats::png::write(grey_image, &grey_file), "grey banner written");
    check(
        !banner_map_png(grey_file, nullptr, indices.data()),
        "a grey picture needs the lobby palette"
    );
    check(
        banner_map_png(grey_file, &palette, indices.data()) && indices[0] == 9 &&
            indices.back() == 9,
        "a grey sample maps to the nearest grey"
    );
    // A 1-bit grey white is level 255, nearest entry 4 (red, by squared distance), not the
    // unscaled 1 of entry 0.
    oa::formats::png::Header one_bit = grey;
    one_bit.bit_depth = 1;
    const std::size_t one_bit_row = (static_cast<std::size_t>(k_ad_banner_width) + 7) / 8;
    const std::vector<uint8_t> one_bit_rows(
        one_bit_row * static_cast<std::size_t>(k_ad_banner_height), 0xff
    );
    oa::formats::png::Image one_bit_image;
    one_bit_image.header = one_bit;
    one_bit_image.rows = one_bit_rows;
    std::vector<uint8_t> one_bit_file;
    check(oa::formats::png::write(one_bit_image, &one_bit_file), "1-bit grey banner written");
    check(
        banner_map_png(one_bit_file, &palette, indices.data()) && indices[0] == 4 &&
            indices.back() == 4,
        "a 1-bit grey sample is scaled before it is matched"
    );

    oa::formats::png::Header colour = grey;
    colour.color_type = oa::formats::png::ColorType::rgb;
    std::vector<uint8_t> rgb_rows(indices.size() * 3);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        rgb_rows[i * 3] = 255;
        rgb_rows[i * 3 + 1] = 0;
        rgb_rows[i * 3 + 2] = 0;
    }
    oa::formats::png::Image rgb_image;
    rgb_image.header = colour;
    rgb_image.rows = rgb_rows;
    std::vector<uint8_t> rgb_file;
    check(oa::formats::png::write(rgb_image, &rgb_file), "colour banner written");
    check(
        !banner_map_png(rgb_file, nullptr, indices.data()),
        "a colour picture needs the lobby palette"
    );
    check(
        banner_map_png(rgb_file, &palette, indices.data()) && indices[0] == 4 &&
            indices.back() == 4,
        "red matches the nearest lobby colour"
    );

    oa::formats::png::Header small = grey;
    small.width = 2;
    small.height = 2;
    std::vector<uint8_t> small_rows(4, 0);
    oa::formats::png::Image small_image;
    small_image.header = small;
    small_image.rows = small_rows;
    std::vector<uint8_t> small_file;
    check(oa::formats::png::write(small_image, &small_file), "small picture written");
    check(
        !banner_map_png(small_file, nullptr, indices.data()), "a picture of another size is skipped"
    );
    check(!banner_map_png({}, nullptr, indices.data()), "bytes that are not a png are skipped");
}

void test_session_override() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "L11-ad-banner-session";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    rover_session_configure(root.string().c_str(), "");
    char url[260];
    char params[260];
    int32_t send_id = 9;
    url[0] = 'Q';
    rover_session_set_ad_server("http://ads.example/banner", 0, "id=1");
    check(
        !rover_session_ad_server(url, sizeof url, &send_id, params, sizeof params) && url[0] == 'Q',
        "send_id 0 suppresses the fetch and does not fall through"
    );
    rover_session_set_ad_server("http://ads.example/banner", 1, "id=1");
    check(
        rover_session_ad_server(url, sizeof url, &send_id, params, sizeof params) &&
            std::strcmp(url, "http://ads.example/banner") == 0 && send_id == 1 &&
            std::strcmp(params, "id=1") == 0,
        "the session reports the ad server a test named"
    );
    rover_session_clear_ad_server();
    url[0] = 'Q';
    check(
        !rover_session_ad_server(url, sizeof url, &send_id, params, sizeof params) && url[0] == 'Q',
        "clearing the override reports no ad server"
    );
    rover_session_set_ad_server("http://ads.example/banner", 1, nullptr);
    rover_session_configure(root.string().c_str(), "");
    url[0] = 'Q';
    check(
        !rover_session_ad_server(url, sizeof url, &send_id, params, sizeof params) && url[0] == 'Q',
        "configuration clears an ad server named for a test"
    );
    std::filesystem::remove_all(root, error);
}

std::vector<uint8_t> shown_pixels(StatusWidget* widget, Handle image) {
    ScopedServices scope(widget);
    Surface surface{};
    std::vector<uint8_t> pixels;
    if (!image_surface(image, &surface) || surface.pixels == nullptr)
        return pixels;
    for (int32_t y = 0; y < surface.height; ++y)
        for (int32_t x = 0; x < surface.width; ++x)
            pixels.push_back(
                surface.pixels
                    [static_cast<std::size_t>(y) * static_cast<std::size_t>(surface.pitch) +
                     static_cast<std::size_t>(x)]
            );
    return pixels;
}

int shown_sample(StatusWidget* widget, const Window* banner) {
    ScopedServices scope(widget);
    if (banner->states.current < 0 || banner->states.current >= banner->states.count)
        return -1;
    Surface surface{};
    const Handle image = banner->states.images[banner->states.current];
    if (!image_surface(image, &surface) || surface.pixels == nullptr || surface.width < 1 ||
        surface.height < 1)
        return -1;
    return surface.pixels
        [static_cast<std::size_t>(surface.height / 2) * static_cast<std::size_t>(surface.pitch) +
         static_cast<std::size_t>(surface.width / 2)];
}

void pulse_banner(StatusWidget* widget, Window* banner, int32_t count) {
    ScopedServices scope(widget);
    for (int32_t i = 0; i < count; ++i)
        banner->ops->pulse(banner);
}

void click_banner(StatusWidget* widget, const Window* banner) {
    const int32_t x = banner->x + banner->width / 2;
    const int32_t y = banner->y + banner->height / 2;
    boneyards_status_widget_event(widget, pointer(MessageCode::left_down, x, y, button_left));
    boneyards_status_widget_event(widget, pointer(MessageCode::left_up, x, y, 0));
}

StatusWidget* enter_boneyards(Data* data, const oa::Palette* lobby = nullptr) {
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.palette = lobby;
    config.service.network_available = service_network;
    config.service.activate = service_activate;
    config.service.connected = service_connected;
    config.service.fetch_file = fetch_failed;
    config.service.set_events = service_events;
    config.service.pump = service_pump;
    config.screens.create = create_screen;
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr, "widget created");
    if (widget == nullptr)
        return nullptr;
    click_banner(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    check(boneyards_status_widget_in_boneyards(widget), "entered the Boneyards");
    return widget;
}

std::vector<uint8_t> sample_frame(StatusWidget* widget) {
    Handle loaded = k_no_handle;
    {
        ScopedServices scope(widget);
        loaded = console_images_load(
            widget->images, "Ad_Banners.Gaf", "Ad_Sample", 0, resource::console_key
        );
    }
    const std::vector<uint8_t> pixels = shown_pixels(widget, loaded);
    {
        ScopedServices scope(widget);
        image_release(loaded);
    }
    return pixels;
}

void test_local_strip(Data* data) {
    rover_session_clear_ad_server();
    rover_session_set_virtual_files(nullptr, nullptr);
    rover_session_set_ad_event_note(note_ad_event, nullptr);
    g_ad_events.clear();
    g_fetches = 0;
    StatusWidget* widget = enter_boneyards(data);
    if (widget == nullptr)
        return;
    MainFrame* frame = widget->system.main_frame;
    Window* banner = frame->ad_banner;
    check(
        frame->base.children.count == 34,
        "the banner is still the one child the Main Frame already had"
    );
    check(
        banner->x == main_frame_layout::banner_x && banner->y == main_frame_layout::banner_y &&
            banner->width == main_frame_layout::banner_width &&
            banner->height == main_frame_layout::banner_height,
        "the strip stays at (152, 18), 468 by 60"
    );
    const std::vector<uint8_t> frame0 = sample_frame(widget);
    const Handle first = banner->states.images[1];
    check(
        !frame0.empty() && shown_pixels(widget, first) == frame0,
        "with no ad server the first sample frame is showing"
    );
    pulse_banner(widget, banner, 900);
    check(banner->states.images[1] == first, "900 pulses still show the first sample");
    pulse_banner(widget, banner, 1);
    check(banner->states.images[1] == first, "pulse 901 shows that same sample again");
    pulse_banner(widget, banner, 901);
    check(
        banner->states.images[1] != first && banner->states.images[1] >= 0,
        "the next sample shows on pulse 1802"
    );
    check(g_fetches == 0, "no ad server fetches nothing");
    click_banner(widget, banner);
    check(g_ad_events.empty(), "the local sample frames send no ad event");
    boneyards_status_widget_destroy(widget);
}

void test_suppressed_and_bad(Data* data) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "L11-ad-banner";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "install", error);
    std::filesystem::create_directories(root / "player", error);
    rover_session_configure((root / "install").string().c_str(), "");
    rover_session_set_player_folder((root / "player").string().c_str());
    Store store;
    store.files["boneyards/ta/ads/ads.def"] = std::vector<uint8_t>{'n', 'o', 'p', 'e'};
    rover_session_set_virtual_files(read_store, &store);
    rover_session_set_ad_server("http://ads.example/banner", 0, "id=1");
    g_fetches = 0;
    StatusWidget* widget = enter_boneyards(data);
    if (widget == nullptr)
        return;
    const std::vector<uint8_t> frame0 = sample_frame(widget);
    check(
        shown_pixels(widget, widget->system.main_frame->ad_banner->states.images[1]) == frame0 &&
            g_fetches == 0 && store.reads["boneyards/ta/ads/ads.def"] == 0,
        "send_id 0 keeps the sample frames and does not fetch"
    );
    boneyards_status_widget_destroy(widget);

    rover_session_set_ad_server("http://ads.example/banner", 1, "id=1");
    widget = enter_boneyards(data);
    if (widget == nullptr)
        return;
    const int reads = store.reads["boneyards/ta/ads/ads.def"];
    check(
        shown_pixels(widget, widget->system.main_frame->ad_banner->states.images[1]) == frame0 &&
            reads >= 1 && g_fetches == 0,
        "a list that is not banners keeps the sample frames"
    );
    pulse_banner(widget, widget->system.main_frame->ad_banner, 20);
    check(
        store.reads["boneyards/ta/ads/ads.def"] == reads && g_fetches == 0,
        "a bad list is not fetched again"
    );
    boneyards_status_widget_destroy(widget);
    rover_session_clear_ad_server();
    rover_session_set_virtual_files(nullptr, nullptr);
    rover_session_set_player_folder(nullptr);
    std::filesystem::remove_all(root, error);
}

void test_server_rotation(Data* data) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "L11-ad-banner";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "install", error);
    std::filesystem::create_directories(root / "player", error);
    rover_session_configure((root / "install").string().c_str(), "");
    rover_session_set_player_folder((root / "player").string().c_str());

    const std::string catalogue =
        "{[Ads<S:command=banner><I:id=1><S:file=/boneyards/ta/ads/red.png><I:seconds=1>"
        "<I:weight=2><S:link=https%6//example.com/a%7b%5c>]}\n"
        "{[Ads<S:command=banner><I:id=2><S:file=/boneyards/ta/ads/blue.png><I:seconds=1>"
        "<I:weight=1>]}\n";
    oa::Palette lobby{};
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i)
        lobby.entries[i] = oa::PaletteEntry{1, 1, 1, 0};
    lobby.entries[7] = oa::PaletteEntry{255, 0, 0, 0};
    lobby.entries[8] = oa::PaletteEntry{0, 0, 255, 0};
    Store store;
    store.files["boneyards/ta/ads/ads.def"] =
        std::vector<uint8_t>(catalogue.begin(), catalogue.end());
    store.files["boneyards/ta/ads/red.png"] = indexed_png(7, png_colours(lobby));
    store.files["boneyards/ta/ads/blue.png"] = indexed_png(8, png_colours(lobby));
    rover_session_set_virtual_files(read_store, &store);
    rover_session_set_ad_server("http://ads.example/banner?cluster=alpha", 1, "id=1");
    screens::registration_set_web_entry({browser_available, browser_open});
    rover_session_set_ad_event_note(note_ad_event, nullptr);
    g_ad_events.clear();
    g_fetches = 0;
    g_open_count = 0;
    g_opened.clear();

    StatusWidget* widget = enter_boneyards(data, &lobby);
    if (widget == nullptr)
        return;
    Window* banner = widget->system.main_frame->ad_banner;
    check(shown_sample(widget, banner) == 7, "the first banner shows at once");
    check(
        g_ad_events.size() == 1 && g_ad_events[0] == ad_event_text(1, kAdEventImpression),
        "the first showing sends one impression"
    );
    check(g_fetches == 0, "a list already in the file layer is not fetched");
    check(
        store.reads["boneyards/ta/ads/red.png"] == 1 &&
            store.reads["boneyards/ta/ads/blue.png"] == 1,
        "each banner file is read once"
    );
    click_banner(widget, banner);
    check(
        g_open_count == 1 && g_opened == "https://example.com/a=b>c",
        "clicking the banner opens its link"
    );
    check(
        g_ad_events.size() == 2 && g_ad_events[1] == ad_event_text(1, kAdEventClick),
        "the click sends one click event"
    );
    pulse_banner(widget, banner, 30);
    check(shown_sample(widget, banner) == 7, "one second of pulses stays on the first banner");
    check(g_ad_events.size() == 2, "pulses while a banner stays up send nothing more");
    pulse_banner(widget, banner, 1);
    check(shown_sample(widget, banner) == 8, "the next banner shows on pulse 31");
    check(
        g_ad_events.size() == 3 && g_ad_events[2] == ad_event_text(2, kAdEventImpression),
        "the next showing sends one impression"
    );
    const int opened = g_open_count;
    const std::size_t events_at_blue = g_ad_events.size();
    click_banner(widget, banner);
    check(g_open_count == opened, "a banner without a link does not open a page");
    check(g_ad_events.size() == events_at_blue, "a click that opens nothing sends no event");
    pulse_banner(widget, banner, 31);
    check(shown_sample(widget, banner) == 7, "weight 2 brings the first banner back");
    check(
        g_ad_events.size() == 4 && g_ad_events[3] == ad_event_text(1, kAdEventImpression),
        "the same banner's next showing sends one more impression"
    );

    const std::filesystem::path ads = root / "player" / "Boneyards" / "ta" / "ads";
    check(
        std::filesystem::is_regular_file(ads / "red.png") &&
            std::filesystem::is_regular_file(ads / "blue.png"),
        "the pictures are cached under the player's Boneyards folder"
    );
    check(
        !std::filesystem::is_regular_file(ads / "ads.def"), "the banner list itself is not cached"
    );
    bool spelled = false;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root / "player", error)) {
        if (entry.path().filename() == "Boneyards")
            spelled = true;
    }
    check(spelled, "the cache folder is spelled Boneyards");
    boneyards_status_widget_destroy(widget);

    const int red_reads = store.reads["boneyards/ta/ads/red.png"];
    const int blue_reads = store.reads["boneyards/ta/ads/blue.png"];
    const int list_reads = store.reads["boneyards/ta/ads/ads.def"];
    const std::size_t events_before_cache = g_ad_events.size();
    widget = enter_boneyards(data, &lobby);
    if (widget == nullptr)
        return;
    check(
        shown_sample(widget, widget->system.main_frame->ad_banner) == 7, "the cached banner shows"
    );
    check(
        g_ad_events.size() == events_before_cache + 1 &&
            g_ad_events.back() == ad_event_text(1, kAdEventImpression),
        "showing the cached banner sends one impression"
    );
    check(
        store.reads["boneyards/ta/ads/red.png"] == red_reads &&
            store.reads["boneyards/ta/ads/blue.png"] == blue_reads,
        "a cached picture is not read from the file layer again"
    );
    check(
        store.reads["boneyards/ta/ads/ads.def"] == list_reads + 1,
        "the list is read again and was not written into the player folder"
    );
    boneyards_status_widget_destroy(widget);

    rover_session_set_virtual_files(nullptr, nullptr);
    g_fetches = 0;
    g_fetch_url[0] = '\0';
    const std::size_t events_before_samples = g_ad_events.size();
    widget = enter_boneyards(data, &lobby);
    if (widget == nullptr)
        return;
    check(
        g_fetches == 1 &&
            std::strcmp(
                g_fetch_url,
                "http://ads.example:80/boneyards/ta/ads/ads.def?cluster=alpha&id=1&lang=en"
            ) == 0 &&
            shown_pixels(widget, widget->system.main_frame->ad_banner->states.images[1]) ==
                sample_frame(widget),
        "a failed fetch keeps the sample frames and asks with the cluster and the language"
    );
    // The list was not cached, so this enter reads nothing from the player folder and asks once.
    pulse_banner(widget, widget->system.main_frame->ad_banner, 40);
    check(g_fetches == 1, "a failed fetch is not repeated");
    check(
        g_ad_events.size() == events_before_samples,
        "the sample frames after a failed fetch send nothing"
    );
    boneyards_status_widget_destroy(widget);

    screens::registration_set_web_entry({});
    rover_session_set_ad_event_note(nullptr, nullptr);
    rover_session_clear_ad_server();
    rover_session_set_player_folder(nullptr);
    std::filesystem::remove_all(root, error);
}

} // namespace

int main() {
    test_parser();
    test_urls();
    test_png_map();
    test_session_override();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }

    const std::filesystem::path root = oa::boneyards_test::require_by_install("the banner tests");
    Data data;
    data.store.mount(root / "by.ccx");
    test_local_strip(&data);
    test_suppressed_and_bad(&data);
    test_server_rotation(&data);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ad banner tests passed\n");
    return 0;
}
