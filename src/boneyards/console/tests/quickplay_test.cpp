// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// QuickPlay: which game types can be chosen, the search (scanner frames, scan
// lines in order, the offer, CANCEL SEARCH, and the five-second timeout), and
// the idle and searching faces against the QuickPlay design. The console is
// opened as on a profile that does not mount the mod. With --frames DIR
// after the mod folder, the idle face and the searching face at 0:07 are
// written there as quickplay-idle.png and quickplay-searching.png.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "console_layer.hpp"
#include "console_state.hpp"
#include "galaxy_fixture.hpp"
#include "oa/boneyards/console/feature_consoles.hpp"
#include "oa/boneyards/console/gui_screen.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/release.hpp"
#include "oa/boneyards/rover_replay.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/quickplay.hpp"
#include "oa/boneyards/service.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/png.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {

int g_failures = 0;

void check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

void expect_text(const char* got, const char* want, int line) {
    const char* shown = got != nullptr ? got : "";
    if (std::strcmp(shown, want) != 0) {
        std::fprintf(stderr, "FAIL line %d: got \"%s\" want \"%s\"\n", line, shown, want);
        ++g_failures;
    }
}

#define EXPECT_TEXT(got, want) expect_text((got), (want), __LINE__)

struct Log {
    std::vector<std::string> sounds;
    std::vector<std::string> calls;
};

Log g_log;
StatusWidget* g_widget = nullptr;

void record_sound(void*, const char* name) {
    g_log.sounds.emplace_back(name != nullptr ? name : "");
}

bool connected(void*) {
    return true;
}

ActivationResult activate(void*) {
    return ActivationResult::ok;
}

void keep_events(void*, const ServiceEvents*) {
}

ServiceResult record_enter(void*, uint32_t) {
    g_log.calls.emplace_back("enter");
    return ServiceResult::pending;
}

ServiceResult record_leave(void*, uint32_t) {
    g_log.calls.emplace_back("leave");
    return ServiceResult::pending;
}

ServiceResult record_get(void*, uint32_t, const char*) {
    g_log.calls.emplace_back("get");
    return ServiceResult::pending;
}

ServiceResult record_join(void*, uint32_t, const char*, const char*) {
    g_log.calls.emplace_back("join");
    return ServiceResult::pending;
}

ServiceResult record_host(void*, const HostGameRequest*) {
    g_log.calls.emplace_back("host");
    return ServiceResult::pending;
}

ServiceResult record_chat(void*, uint32_t, uint32_t, uint32_t, const char*) {
    g_log.calls.emplace_back("chat");
    return ServiceResult::pending;
}

ServiceResult record_page(void*, const char*, const char*) {
    g_log.calls.emplace_back("page");
    return ServiceResult::pending;
}

ServiceResult record_locate(void*, const char*, LocateCallback, void*) {
    g_log.calls.emplace_back("locate");
    return ServiceResult::pending;
}

ServiceResult record_room(void*, uint32_t, uint32_t, const char*, const char*) {
    g_log.calls.emplace_back("room");
    return ServiceResult::pending;
}

ServiceResult record_create_room(void*, uint32_t, const char*, const char*, const char*) {
    g_log.calls.emplace_back("create-room");
    return ServiceResult::pending;
}

ServiceResult record_profile(void*, uint32_t, ProfileCallback, LadderCallback, void*) {
    g_log.calls.emplace_back("profile");
    return ServiceResult::pending;
}

ServiceResult record_fetch(void*, const char*, char*, std::size_t) {
    g_log.calls.emplace_back("fetch");
    return ServiceResult::pending;
}

void record_allegiance(void*) {
    g_log.calls.emplace_back("allegiance");
}

void record_census(void*, uint32_t) {
    g_log.calls.emplace_back("census");
}

int32_t heard_count(const char* name) {
    int32_t count = 0;
    for (const std::string& sound : g_log.sounds)
        count += sound == name ? 1 : 0;
    return count;
}

Message pointer(MessageCode code, int32_t x, int32_t y, uint32_t buttons) {
    Message msg{};
    msg.code = code;
    msg.x = x;
    msg.y = y;
    msg.buttons = buttons;
    return msg;
}

void click_window(const Window* w) {
    if (w == nullptr)
        return;
    const int32_t x = w->x + w->width / 2;
    const int32_t y = w->y + w->height / 2;
    boneyards_status_widget_event(g_widget, pointer(MessageCode::left_down, x, y, button_left));
    boneyards_status_widget_event(g_widget, pointer(MessageCode::left_up, x, y, 0));
}

void advance(Window* console, int32_t ms) {
    ScopedServices scope(g_widget);
    quickplay_console_advance(console, ms);
}

void tick(int32_t cycles) {
    for (int32_t i = 0; i < cycles; ++i)
        boneyards_status_widget_tick(g_widget);
}

void finish_slide(const MainFrame* frame) {
    for (int32_t i = 0; frame->console->sliding != 0 && i < 100; ++i)
        tick(1);
}

std::string text_of(Window* w) {
    ScopedServices scope(g_widget);
    const char* text = w != nullptr && w->ops != nullptr ? w->ops->state_text(w, -1) : nullptr;
    return text != nullptr ? text : "";
}

Window* named(Window* root, const char* name) {
    Window* found = root != nullptr ? window_find_named(root, name, nullptr) : nullptr;
    if (found == nullptr) {
        std::fprintf(stderr, "FAIL missing window %s\n", name);
        ++g_failures;
    }
    return found;
}

void expect_box(
    const Window* w, int32_t x, int32_t y, int32_t width, int32_t height, const char* name
) {
    if (w == nullptr)
        return;
    if (w->x != x || w->y != y || w->width != width || w->height != height) {
        std::fprintf(
            stderr,
            "FAIL %s at %d,%d %dx%d want %d,%d %dx%d\n",
            name,
            w->x,
            w->y,
            w->width,
            w->height,
            x,
            y,
            width,
            height
        );
        ++g_failures;
    }
}

void expect_visible(Window* root, const char* name, bool visible) {
    Window* w = named(root, name);
    if (w == nullptr)
        return;
    if ((w->visible != 0) != visible) {
        std::fprintf(stderr, "FAIL %s visibility\n", name);
        ++g_failures;
    }
}

void expect_state(Window* root, const char* name, int32_t state) {
    Window* w = named(root, name);
    if (w == nullptr)
        return;
    if (w->states.current != state) {
        std::fprintf(stderr, "FAIL %s state %d want %d\n", name, w->states.current, state);
        ++g_failures;
    }
}

void expect_at(const Window* w, int32_t x, int32_t y, const char* name) {
    if (w == nullptr)
        return;
    if (w->x != x || w->y != y) {
        std::fprintf(stderr, "FAIL %s at %d,%d want %d,%d\n", name, w->x, w->y, x, y);
        ++g_failures;
    }
}

struct Data {
    oa::AssetStore store{oa::boneyards_test::by_install()};
    oa::Palette palette{};
};

bool read_asset(void* user, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<Data*>(user)->store.read(path).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Widens Function_Button_Large to the 160-pixel QuickPlay button: 12 pixels
// stay at each end and the middle is repeated. Written out here so the
// comparison does not call the console.
Handle widen_large_button(Handle source) {
    constexpr int32_t k_cap = 12;
    constexpr int32_t k_width = 160;
    Surface src{};
    if (source < 0 || !image_surface(source, &src) || src.pixels == nullptr ||
        src.width < k_cap * 2)
        return k_no_handle;
    const Handle dest = image_create_blank(k_width, src.height, 9, 9);
    Surface out{};
    if (dest < 0 || !image_surface(dest, &out) || out.pixels == nullptr)
        return dest;
    const int32_t middle = src.width - k_cap * 2;
    const int32_t rows = src.height < out.height ? src.height : out.height;
    auto copy = [&](int32_t from, int32_t count, int32_t to) {
        for (int32_t y = 0; y < rows; ++y) {
            std::memcpy(
                out.pixels + static_cast<std::ptrdiff_t>(y) * out.pitch + to,
                src.pixels + static_cast<std::ptrdiff_t>(y) * src.pitch + from,
                static_cast<std::size_t>(count)
            );
        }
    };
    copy(0, k_cap, 0);
    for (int32_t x = k_cap; x < k_width - k_cap;) {
        const int32_t count = middle < k_width - k_cap - x ? middle : k_width - k_cap - x;
        copy(k_cap, count, x);
        x += middle;
    }
    copy(src.width - k_cap, k_cap, k_width - k_cap);
    return dest;
}

std::vector<uint8_t> frame_pixels() {
    std::vector<uint8_t> pixels(640 * 480, 0);
    Surface target{};
    target.width = 640;
    target.height = 480;
    target.pitch = 640;
    target.pixels = pixels.data();
    target.clip = oa::Rect32{0, 0, 639, 479};
    boneyards_status_widget_draw(g_widget, &target);
    return pixels;
}

// The folder --frames names, where the faces are written as pictures to
// compare with the design. Empty writes none.
std::filesystem::path g_frames;

// Writes a drawn frame as an indexed PNG in the widget's palette.
void write_picture(const std::vector<uint8_t>& drawn, const char* name) {
    if (g_frames.empty() || g_widget == nullptr || g_widget->config.palette == nullptr)
        return;
    std::vector<oa::formats::png::Rgb> colors(static_cast<std::size_t>(OA_PALETTE_COLORS));
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i) {
        const oa::PaletteEntry& entry = g_widget->config.palette->entries[i];
        colors[static_cast<std::size_t>(i)] = oa::formats::png::Rgb{entry.r, entry.g, entry.b};
    }
    oa::formats::png::Image image;
    image.header.width = 640;
    image.header.height = 480;
    image.header.bit_depth = 8;
    image.header.color_type = oa::formats::png::ColorType::palette;
    image.palette = colors;
    image.rows = drawn;
    std::vector<uint8_t> bytes;
    CHECK(oa::formats::png::write(image, &bytes));
    std::error_code error;
    std::filesystem::create_directories(g_frames, error);
    std::ofstream out(g_frames / name, std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    CHECK(out.good());
}

// Opaque pixels of `image` drawn at (x, y), origin included. `key` is skipped.
void expect_picture(
    const std::vector<uint8_t>& drawn,
    Handle image,
    int32_t x,
    int32_t y,
    int32_t key,
    const char* what
) {
    ScopedServices scope(g_widget);
    Surface surface{};
    if (image < 0 || !image_surface(image, &surface) || surface.pixels == nullptr) {
        std::fprintf(stderr, "FAIL %s: picture missing\n", what);
        ++g_failures;
        return;
    }
    int32_t opaque = 0;
    int32_t bad = 0;
    int32_t bad_x = 0;
    int32_t bad_y = 0;
    int got = 0;
    int want = 0;
    for (int32_t row = 0; row < surface.height; ++row) {
        const uint8_t* in = surface.pixels + static_cast<std::ptrdiff_t>(row) * surface.pitch;
        for (int32_t col = 0; col < surface.width; ++col) {
            if (in[col] == key)
                continue;
            ++opaque;
            const int32_t sx = x - surface.origin_x + col;
            const int32_t sy = y - surface.origin_y + row;
            const int pixel =
                sx >= 0 && sy >= 0 && sx < 640 && sy < 480 ? drawn[sy * 640 + sx] : -1;
            if (pixel != in[col]) {
                if (bad == 0) {
                    bad_x = sx;
                    bad_y = sy;
                    got = pixel;
                    want = in[col];
                }
                ++bad;
            }
        }
    }
    if (opaque == 0 || bad != 0) {
        std::fprintf(
            stderr,
            "FAIL %s: opaque %d mismatches %d first %d,%d got %d want %d\n",
            what,
            opaque,
            bad,
            bad_x,
            bad_y,
            got,
            want
        );
        ++g_failures;
    }
}

void expect_caps(
    const std::vector<uint8_t>& drawn, Handle widened, int32_t x, int32_t y, const char* what
) {
    ScopedServices scope(g_widget);
    Surface surface{};
    if (widened < 0 || !image_surface(widened, &surface) || surface.pixels == nullptr) {
        std::fprintf(stderr, "FAIL %s: widened picture missing\n", what);
        ++g_failures;
        return;
    }
    int32_t opaque = 0;
    int32_t bad = 0;
    for (int32_t row = 0; row < surface.height; ++row) {
        const uint8_t* in = surface.pixels + static_cast<std::ptrdiff_t>(row) * surface.pitch;
        for (int32_t col = 0; col < surface.width; ++col) {
            if (col >= 12 && col < 148)
                continue;
            if (in[col] == 9)
                continue;
            ++opaque;
            const int32_t sx = x - surface.origin_x + col;
            const int32_t sy = y - surface.origin_y + row;
            const int pixel =
                sx >= 0 && sy >= 0 && sx < 640 && sy < 480 ? drawn[sy * 640 + sx] : -1;
            if (pixel != in[col])
                ++bad;
        }
    }
    if (opaque == 0 || bad != 0) {
        std::fprintf(stderr, "FAIL %s caps: opaque %d mismatches %d\n", what, opaque, bad);
        ++g_failures;
    }
}

void expect_same_image(Handle got, Handle want, const char* what) {
    ScopedServices scope(g_widget);
    Surface a{};
    Surface b{};
    if (got < 0 || want < 0 || !image_surface(got, &a) || !image_surface(want, &b) ||
        a.pixels == nullptr || b.pixels == nullptr || a.width != b.width || a.height != b.height) {
        std::fprintf(stderr, "FAIL %s: stretched pictures differ in size\n", what);
        ++g_failures;
        return;
    }
    for (int32_t row = 0; row < a.height; ++row) {
        const uint8_t* pa = a.pixels + static_cast<std::ptrdiff_t>(row) * a.pitch;
        const uint8_t* pb = b.pixels + static_cast<std::ptrdiff_t>(row) * b.pitch;
        if (std::memcmp(pa, pb, static_cast<std::size_t>(a.width)) != 0) {
            std::fprintf(stderr, "FAIL %s: stretched pixels differ on row %d\n", what, row);
            ++g_failures;
            return;
        }
    }
}

// The console glass at a screen pixel: the Green_Screen border where it is
// opaque, otherwise the green console layer's remapped war map.
int expected_glass(const Surface& border, int32_t x, int32_t y) {
    const ConsoleLayer* layer =
        g_widget->system.main_frame != nullptr ? g_widget->system.main_frame->console : nullptr;
    const int32_t col = x - main_frame_layout::console_x;
    const int32_t row = y - main_frame_layout::console_y;
    if (layer == nullptr || layer->green == nullptr || col < 0 || row < 0 || col >= border.width ||
        row >= border.height)
        return -1;
    const uint8_t edge = border.pixels[static_cast<std::ptrdiff_t>(row) * border.pitch + col];
    if (edge != 9)
        return edge;
    const Surface& glass = layer->green->surface;
    if (col >= glass.width || row >= glass.height)
        return -1;
    return glass.pixels[static_cast<std::ptrdiff_t>(row) * glass.pitch + col];
}

// QuickPlay draws no field of its own: the green console layer, the war map
// under the Green_Screen border, shows wherever no control is drawn. The
// bottom bevel and the left column under the blurb are sampled, and the
// spots inside the two wells, which are outlines.
void expect_glass(const std::vector<uint8_t>& drawn) {
    ScopedServices scope(g_widget);
    const Handle green = image_load("Widgets.gaf", "Green_Screen", 0, 9);
    Surface border{};
    if (green < 0 || !image_surface(green, &border) || border.pixels == nullptr) {
        std::fprintf(stderr, "FAIL Green_Screen missing\n");
        ++g_failures;
        if (green >= 0)
            image_release(green);
        return;
    }
    int32_t opaque = 0;
    int32_t keyed = 0;
    int32_t bad = 0;
    auto sample = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        for (int32_t y = y0; y < y1; ++y) {
            for (int32_t x = x0; x < x1; ++x) {
                const int want = expected_glass(border, x, y);
                const int32_t col = x - main_frame_layout::console_x;
                const int32_t row = y - main_frame_layout::console_y;
                if (want >= 0 &&
                    border.pixels[static_cast<std::ptrdiff_t>(row) * border.pitch + col] == 9)
                    ++keyed;
                else
                    ++opaque;
                if (want < 0 || drawn[y * 640 + x] != want)
                    ++bad;
            }
        }
    };
    sample(40, 432, 80, 439);
    sample(30, 300, 140, 420);
    sample(426, 141, 429, 144);
    if (opaque == 0 || bad != 0 || keyed == 0) {
        std::fprintf(
            stderr, "FAIL console glass: opaque %d keyed %d mismatches %d\n", opaque, keyed, bad
        );
        ++g_failures;
    }
    if (green >= 0)
        image_release(green);
}

// The searching face's log well is an outline: the glass shows inside it.
void expect_log_glass(const std::vector<uint8_t>& drawn) {
    ScopedServices scope(g_widget);
    const Handle green = image_load("Widgets.gaf", "Green_Screen", 0, 9);
    Surface border{};
    if (green >= 0 && image_surface(green, &border) && border.pixels != nullptr) {
        for (int32_t y = 277; y < 279; ++y) {
            for (int32_t x = 159; x < 162; ++x)
                CHECK(drawn[y * 640 + x] == expected_glass(border, x, y));
        }
    } else {
        CHECK(false);
    }
    if (green >= 0)
        image_release(green);
}

void test_model() {
    CHECK(quickplay_console_model(nullptr) == nullptr);
    quickplay_console_advance(nullptr, 50);
    screens::quickplay_init(nullptr);
    screens::quickplay_find(nullptr);
    screens::quickplay_cancel(nullptr);
    screens::quickplay_advance(nullptr, 10);

    screens::QuickPlay play{};
    screens::quickplay_init(&play);
    CHECK(play.selected == screens::kQuickPlayDuel);
    CHECK(play.where == screens::QuickPlayWhere::any_planet);
    CHECK(play.skill == screens::QuickPlaySkill::seasoned);
    CHECK(play.side == screens::QuickPlaySide::either);
    CHECK(play.prefer_favorite);
    EXPECT_TEXT(play.favorite, "Lava Alley");
    CHECK(!play.searching && play.elapsed_ms == 0 && play.requests_sent == 0 && !play.offer_open);
    CHECK(play.allowed[screens::kQuickPlayDuel]);
    CHECK(play.allowed[screens::kQuickPlayWinnerTakesAll]);
    CHECK(play.allowed[screens::kQuickPlayForFun]);
    CHECK(!play.allowed[screens::kQuickPlayGalacticWar]);
    CHECK(!play.allowed[screens::kQuickPlayWarTraining]);
    CHECK(!screens::quickplay_select_type(&play, screens::kQuickPlayGalacticWar));
    CHECK(play.selected == screens::kQuickPlayDuel);
    CHECK(!screens::quickplay_select_type(&play, 9));
    CHECK(!screens::quickplay_type_allowed(nullptr, 0));
    EXPECT_TEXT(screens::quickplay_type_name(screens::kQuickPlayDuel), "Duel");
    EXPECT_TEXT(
        screens::quickplay_type_name(screens::kQuickPlayWinnerTakesAll), "Winner Takes All"
    );
    EXPECT_TEXT(screens::quickplay_type_name(screens::kQuickPlayForFun), "For Fun");
    EXPECT_TEXT(screens::quickplay_type_name(screens::kQuickPlayGalacticWar), "Galactic War");
    EXPECT_TEXT(screens::quickplay_type_name(screens::kQuickPlayWarTraining), "War Training");
    EXPECT_TEXT(screens::quickplay_type_name(5), "");
    EXPECT_TEXT(screens::quickplay_blurb(screens::kQuickPlayDuel, 0), "Two commanders, one map,");
    EXPECT_TEXT(screens::quickplay_blurb(screens::kQuickPlayDuel, 1), "no allies. Ranked on the");
    EXPECT_TEXT(screens::quickplay_blurb(screens::kQuickPlayDuel, 2), "Duel ladder.");
    CHECK(screens::quickplay_select_type(&play, screens::kQuickPlayForFun));
    EXPECT_TEXT(screens::quickplay_blurb(play.selected, 0), "A battle that is not");
    CHECK(
        std::strcmp(
            screens::quickplay_blurb(play.selected, 0),
            screens::quickplay_blurb(screens::kQuickPlayDuel, 0)
        ) != 0
    );

    struct Place {
        bool has;
        char side;
        char allegiance;
        bool war;
        bool training;
    };

    const Place rows[] = {
        {false, 0, 'U', false, false},
        {true, 'I', 'A', true, false},
        {true, 'i', 'C', true, false},
        {true, 'I', 'U', false, false},
        {true, 'C', 'A', false, false},
        {true, 'C', 'C', false, true},
        {true, 'A', 'A', false, true},
        {true, 'a', 'A', false, true},
        {true, 'c', 'C', false, true},
        {true, 'A', 'C', false, false},
    };
    for (const Place& row : rows) {
        screens::quickplay_set_place(&play, row.has, row.side, row.allegiance);
        CHECK(play.allowed[screens::kQuickPlayDuel]);
        CHECK(play.allowed[screens::kQuickPlayWinnerTakesAll]);
        CHECK(play.allowed[screens::kQuickPlayForFun]);
        if (play.allowed[screens::kQuickPlayGalacticWar] != row.war ||
            play.allowed[screens::kQuickPlayWarTraining] != row.training) {
            std::fprintf(
                stderr,
                "FAIL place side '%c' allegiance '%c' war %d training %d\n",
                row.side,
                row.allegiance,
                play.allowed[screens::kQuickPlayGalacticWar] ? 1 : 0,
                play.allowed[screens::kQuickPlayWarTraining] ? 1 : 0
            );
            ++g_failures;
        }
    }

    screens::quickplay_set_place(&play, true, 'A', 'A');
    CHECK(screens::quickplay_select_type(&play, screens::kQuickPlayWarTraining));
    screens::quickplay_set_place(&play, false, 0, 'U');
    CHECK(play.selected == screens::kQuickPlayDuel);

    screens::quickplay_set_favorite(&play, nullptr);
    EXPECT_TEXT(play.favorite, "Lava Alley");
    screens::quickplay_set_favorite(&play, "");
    EXPECT_TEXT(play.favorite, "Lava Alley");
    screens::quickplay_set_favorite(&play, "Coast to Coast");
    char caption[screens::kQuickPlayTextCapacity];
    screens::quickplay_favorite_caption(&play, caption, sizeof caption);
    EXPECT_TEXT(caption, "Prefer my favorite: Coast to Coast");
    screens::quickplay_set_favorite(&play, "Lava Alley");

    CHECK(screens::quickplay_scanner_frame(&play) == 0);
    CHECK(screens::quickplay_visible_lines(&play) == 0);
    screens::quickplay_find(&play);
    CHECK(play.searching && play.elapsed_ms == 0 && play.requests_sent == 0 && !play.offer_open);
    CHECK(screens::quickplay_scanner_frame(&play) == 0);
    CHECK(screens::quickplay_visible_lines(&play) == 1);
    CHECK(screens::quickplay_meter_frame(&play) == screens::kQuickPlayMeterFrame);
    char line[screens::kQuickPlayTextCapacity];
    screens::quickplay_scan_line(&play, 0, line, sizeof line);
    EXPECT_TEXT(line, "Scanning 0 planets for a Duel at Seasoned skill...");
    screens::quickplay_advance(&play, 110);
    screens::quickplay_find(&play);
    CHECK(play.elapsed_ms == 110);
    CHECK(screens::quickplay_scanner_frame(&play) == 1);
    screens::quickplay_advance(&play, 110);
    CHECK(screens::quickplay_scanner_frame(&play) == 2);
    screens::quickplay_advance(&play, 660);
    CHECK(play.elapsed_ms == 880 && screens::quickplay_scanner_frame(&play) == 0);
    screens::quickplay_advance(&play, 7000 - 880);
    CHECK(play.elapsed_ms == 7000);
    CHECK(screens::quickplay_scanner_frame(&play) == 7);
    char timer[16];
    screens::quickplay_timer(&play, timer, sizeof timer);
    EXPECT_TEXT(timer, "0:07");
    CHECK(screens::quickplay_visible_lines(&play) == 1);
    screens::quickplay_scan_line(&play, 1, line, sizeof line);
    EXPECT_TEXT(line, "");
    CHECK(std::strstr(line, "Battle found") == nullptr);
    CHECK(play.requests_sent == 0 && !play.offer_open);
    // The model stays on the search. The console is what ends it when this flag is set.
    CHECK(screens::quickplay_status_overdue(&play));
    screens::quickplay_cancel(&play);
    CHECK(!play.searching && play.elapsed_ms == 0 && play.requests_sent == 0);

    play.where = screens::QuickPlayWhere::fuel_dump;
    play.skill = screens::QuickPlaySkill::expert;
    play.selected = screens::kQuickPlayWinnerTakesAll;
    screens::quickplay_find(&play);
    screens::quickplay_scan_line(&play, 0, line, sizeof line);
    EXPECT_TEXT(line, "Scanning Fuel Dump for a Winner Takes All at Expert skill...");
    QuickPlayScanNotice skipped{};
    skipped.seq = 1;
    skipped.kind = 2;
    skipped.reason = 0;
    skipped.skill = 2;
    std::snprintf(skipped.planet, sizeof skipped.planet, "Fuel Dump");
    std::snprintf(skipped.battle, sizeof skipped.battle, "Brawl at Dawn");
    screens::quickplay_apply_scan(&play, &skipped);
    CHECK(screens::quickplay_visible_lines(&play) == 1);
    screens::quickplay_advance(&play, 1400);
    CHECK(screens::quickplay_visible_lines(&play) == 2);
    screens::quickplay_scan_line(&play, 1, line, sizeof line);
    EXPECT_TEXT(line, "Fuel Dump: Brawl at Dawn wants Expert - skipped");
    screens::quickplay_advance(&play, 7000 - 1400);
    CHECK(screens::quickplay_visible_lines(&play) == 2);
    screens::quickplay_timer(&play, timer, sizeof timer);
    EXPECT_TEXT(timer, "0:07");
    screens::quickplay_advance(&play, 60000 - 7000);
    screens::quickplay_timer(&play, timer, sizeof timer);
    EXPECT_TEXT(timer, "1:00");
    CHECK(play.requests_sent == 0 && !play.offer_open);
}

void store_scan(
    screens::QuickPlay* play,
    int32_t seq,
    int32_t kind,
    int32_t reason,
    const char* planet,
    const char* battle,
    int32_t game_type,
    int32_t skill,
    int32_t seats,
    int32_t seats_max
) {
    QuickPlayScanNotice scan{};
    scan.seq = seq;
    scan.kind = kind;
    scan.reason = reason;
    scan.game_type = game_type;
    scan.skill = skill;
    scan.seats = seats;
    scan.seats_max = seats_max;
    std::snprintf(scan.planet, sizeof scan.planet, "%s", planet != nullptr ? planet : "");
    std::snprintf(scan.battle, sizeof scan.battle, "%s", battle != nullptr ? battle : "");
    screens::quickplay_apply_scan(play, &scan);
}

QuickPlayFoundNotice join_offer() {
    QuickPlayFoundNotice found{};
    std::snprintf(found.offer_id, sizeof found.offer_id, "a1b2c3d4e5f607");
    std::snprintf(found.game_id, sizeof found.game_id, "c0ffee0a11b223");
    found.game_known = true;
    found.role = 0;
    found.area_id = 0x1a2;
    found.game_type = 1;
    found.skill = 1;
    std::snprintf(found.name, sizeof found.name, "Rumble in the Rubble");
    std::snprintf(found.planet, sizeof found.planet, "Lava Alley");
    return found;
}

QuickPlayFoundNotice host_offer(int32_t seats) {
    QuickPlayFoundNotice found{};
    std::snprintf(found.offer_id, sizeof found.offer_id, "a1b2c3d4e5f607");
    found.role = 1;
    found.area_id = 2;
    found.game_type = 1;
    found.skill = 1;
    found.seats = seats;
    found.on_death = 2;
    std::snprintf(found.name, sizeof found.name, "ArmSniper");
    std::snprintf(found.planet, sizeof found.planet, "Empyrrean");
    return found;
}

void test_search_records() {
    screens::QuickPlay play{};
    screens::quickplay_init(&play);
    QuickPlayQuery query{};
    screens::quickplay_query(&play, 2, true, &query);
    CHECK(query.game_type == 1 && query.where == 0 && query.skill == 1 && query.side == 2);
    CHECK(query.prefer_favorite == 1 && query.area_known && query.area_id == 2);
    EXPECT_TEXT(query.favorite, "Lava Alley");
    play.prefer_favorite = false;
    play.selected = screens::kQuickPlayGalacticWar;
    screens::quickplay_query(&play, 0x1a2, true, &query);
    CHECK(query.game_type == 4 && query.prefer_favorite == 0 && query.favorite[0] == '\0');
    CHECK(query.area_known && query.area_id == 0x1a2);
    play.selected = screens::kQuickPlayForFun;
    screens::quickplay_query(&play, 0, false, &query);
    CHECK(query.game_type == 0 && !query.area_known);
    play.selected = screens::kQuickPlayWinnerTakesAll;
    screens::quickplay_query(&play, 0, false, &query);
    CHECK(query.game_type == 2);
    play.selected = screens::kQuickPlayWarTraining;
    screens::quickplay_query(&play, 0, false, &query);
    CHECK(query.game_type == 7);

    screens::quickplay_init(&play);
    screens::quickplay_find(&play);
    CHECK(!screens::quickplay_status_overdue(&play));
    screens::quickplay_advance(&play, screens::kQuickPlayStatusWaitMs - 1);
    CHECK(!screens::quickplay_status_overdue(&play));
    screens::quickplay_advance(&play, 1);
    CHECK(screens::quickplay_status_overdue(&play));
    play.requests_sent = 3;
    screens::quickplay_cancel(&play);
    CHECK(!play.searching && play.requests_sent == 3 && !screens::quickplay_status_overdue(&play));

    screens::quickplay_find(&play);
    QuickPlayStatusNotice status{};
    status.queued = 1;
    status.meter = 12;
    status.fits = 12;
    status.planets = 25;
    screens::QuickPlayEffect effect = screens::quickplay_apply_status(&play, &status);
    CHECK(!play.awaiting_status && play.has_status && play.searching);
    CHECK(screens::quickplay_meter_frame(&play) == screens::kQuickPlayMeterLast);
    CHECK(!effect.show_box && !effect.leave_search);
    char line[screens::kQuickPlayTextCapacity];
    screens::quickplay_scan_line(&play, 0, line, sizeof line);
    EXPECT_TEXT(line, "Scanning 25 planets for a Duel at Seasoned skill...");
    status.meter = -3;
    screens::quickplay_apply_status(&play, &status);
    CHECK(screens::quickplay_meter_frame(&play) == 0);

    store_scan(&play, 2, 1, 0, "Altair", "", 0, 0, 0, 0);
    store_scan(&play, 1, 1, 0, "Ignored", "", 0, 0, 0, 0);
    store_scan(&play, 3, 2, 0, "Fuel Dump", "Brawl at Dawn", 0, 2, 0, 0);
    store_scan(&play, 4, 2, 1, "Deadman Alley", "Last Stand", 4, 0, 0, 0);
    store_scan(&play, 5, 2, 2, "Core Prime", "Locked", 0, 0, 0, 0);
    store_scan(&play, 6, 3, 0, "Lava Alley", "Rumble in the Rubble", 1, 1, 1, 2);
    CHECK(screens::quickplay_visible_lines(&play) == 1);
    screens::quickplay_advance(&play, 1400);
    CHECK(screens::quickplay_visible_lines(&play) == 2);
    screens::quickplay_scan_line(&play, 1, line, sizeof line);
    EXPECT_TEXT(line, "Altair: no forming battles");
    screens::quickplay_advance(&play, 1400);
    screens::quickplay_scan_line(&play, 2, line, sizeof line);
    EXPECT_TEXT(line, "Fuel Dump: Brawl at Dawn wants Expert - skipped");
    screens::quickplay_advance(&play, 1400);
    screens::quickplay_scan_line(&play, 3, line, sizeof line);
    EXPECT_TEXT(line, "Deadman Alley: Last Stand is Galactic War - skipped");
    screens::quickplay_advance(&play, 1400);
    CHECK(screens::quickplay_visible_lines(&play) == 5);
    screens::quickplay_scan_line(&play, 4, line, sizeof line);
    EXPECT_TEXT(line, "Core Prime: Locked - skipped");
    screens::quickplay_advance(&play, 1400);
    CHECK(screens::quickplay_visible_lines(&play) == 5);
    screens::quickplay_scan_line(&play, 1, line, sizeof line);
    EXPECT_TEXT(line, "Fuel Dump: Brawl at Dawn wants Expert - skipped");
    screens::quickplay_scan_line(&play, 4, line, sizeof line);
    EXPECT_TEXT(line, "Lava Alley: Rumble in the Rubble, 1 of 2 seats, Seasoned");
    screens::quickplay_advance(&play, 1400);
    screens::quickplay_scan_line(&play, 4, line, sizeof line);
    EXPECT_TEXT(line, "Lava Alley: Rumble in the Rubble, 1 of 2 seats, Seasoned");

    const QuickPlayFoundNotice join = join_offer();
    effect = screens::quickplay_apply_found(&play, &join);
    CHECK(effect.show_offer && play.offer_open);
    char offer[192];
    screens::quickplay_offer_line(&play, offer, sizeof offer);
    CHECK(std::strcmp(offer, "Rumble in the Rubble\nDuel on Lava Alley\nJoin now?") == 0);
    const int32_t elapsed = play.elapsed_ms;
    effect = screens::quickplay_decide(&play, 0);
    CHECK(!effect.launch_join && !effect.launch_host && effect.close_offer);
    CHECK(play.searching && !play.offer_open && play.elapsed_ms == elapsed);
    EXPECT_TEXT(play.declined_game_id, "c0ffee0a11b223");
    effect = screens::quickplay_apply_found(&play, &join);
    CHECK(!effect.show_offer && !play.offer_open);

    const QuickPlayFoundNotice host = host_offer(2);
    effect = screens::quickplay_apply_found(&play, &host);
    CHECK(effect.show_offer && play.offer_open);
    screens::quickplay_offer_line(&play, offer, sizeof offer);
    CHECK(std::strcmp(offer, "ArmSniper\nDuel on Empyrrean\nNo games available, host one?") == 0);
    effect = screens::quickplay_decide(&play, 1);
    // Yes ends the search: the battle it hosts leaves the queue on the server too.
    CHECK(effect.launch_host && !effect.launch_join && effect.leave_search);
    CHECK(!play.searching && play.launch_pending && !play.offer_open);
    CHECK(!screens::quickplay_status_overdue(&play));
    HostGameRequest request{};
    CHECK(screens::quickplay_host_request(&play, &request));
    CHECK(request.players == 0 && request.password == nullptr);
    CHECK(request.skill == 1 && request.commander_death == 2);
    CHECK(request.game_mode == 1 && request.area_id == 2);
    CHECK(std::strcmp(request.name, "ArmSniper") == 0);
    CHECK(screens::quickplay_players_radio(3) == 1);
    CHECK(screens::quickplay_players_radio(4) == 2);
    CHECK(screens::quickplay_players_radio(9) == 0);
    play.offer_seats = 3;
    CHECK(screens::quickplay_host_request(&play, &request) && request.players == 1);
    play.offer_seats = 9;
    CHECK(screens::quickplay_host_request(&play, &request) && request.players == 0);

    // A host the server refused goes on searching: its next status puts the card back on it.
    store_scan(&play, 40, 1, 0, "Gods of War", "", 0, 0, 0, 0);
    CHECK(play.last_seq == 40);
    status = {};
    status.queued = 1;
    status.planets = 25;
    effect = screens::quickplay_apply_status(&play, &status);
    CHECK(play.searching && !play.launch_pending && !effect.leave_search && !effect.show_box);

    screens::quickplay_cancel(&play);
    screens::quickplay_find(&play);
    CHECK(!play.launch_pending);
    const QuickPlayFoundNotice again = join_offer();
    screens::quickplay_apply_found(&play, &again);
    effect = screens::quickplay_decide(&play, 1);
    CHECK(effect.launch_join && !effect.launch_host && effect.leave_search);
    CHECK(!play.searching && play.launch_pending && !play.offer_open);
    EXPECT_TEXT(play.offer_game_id, "c0ffee0a11b223");

    status = {};
    status.result = 5;
    status.queued = 0;
    play.offer_open = true;
    play.offer_game_known = true;
    std::snprintf(play.offer_game_id, sizeof play.offer_game_id, "c0ffee0a11b223");
    effect = screens::quickplay_apply_status(&play, &status);
    CHECK(effect.show_box && effect.box_result == 5 && effect.close_offer);
    CHECK(play.searching && !play.offer_open);
    EXPECT_TEXT(play.declined_game_id, "c0ffee0a11b223");

    status.result = 4;
    status.queued = 0;
    effect = screens::quickplay_apply_status(&play, &status);
    CHECK(effect.leave_search && effect.show_box && effect.box_result == 4 && !play.searching);
    status.result = 1;
    status.queued = 0;
    screens::quickplay_find(&play);
    effect = screens::quickplay_apply_status(&play, &status);
    CHECK(effect.box_result == 1 && !play.searching);
    CHECK(screens::quickplay_result_text(0) == nullptr);
    CHECK(screens::quickplay_result_text(3) == nullptr);
    EXPECT_TEXT(screens::quickplay_result_text(1), "QuickPlay is not open for this account.");
    EXPECT_TEXT(screens::quickplay_result_text(2), "That battle is not allowed on this planet.");
    EXPECT_TEXT(screens::quickplay_result_text(5), "That battle has filled.");
    EXPECT_TEXT(screens::quickplay_result_text(6), "Fuel Dump is not in this sector.");
    EXPECT_TEXT(screens::quickplay_result_text(7), "The queue dropped this search.");
    EXPECT_TEXT(screens::quickplay_result_text(8), "Those settings cannot be queued.");
    EXPECT_TEXT(screens::quickplay_unavailable_text(), "The search could not be started.");
    EXPECT_TEXT(screens::quickplay_result_text(4), "You are already in a battle.");
}

void reread(Window* console) {
    console->visible = 0;
    {
        ScopedServices scope(g_widget);
        console->ops->pulse(console);
    }
    console->visible = 1;
    {
        ScopedServices scope(g_widget);
        console->ops->pulse(console);
    }
}

void expect_stretched(Window* button, const char* what) {
    ScopedServices scope(g_widget);
    for (int32_t state = 0; state < 4; ++state) {
        const Handle source = image_load("Widgets.gaf", "Function_Button_Large", state, 9);
        const Handle widened = widen_large_button(source);
        char label[64];
        std::snprintf(label, sizeof label, "%s state %d", what, state);
        const Handle face = button != nullptr && state < button->states.count
                                ? button->states.images[state]
                                : k_no_handle;
        expect_same_image(face, widened, label);
        if (source >= 0)
            image_release(source);
        if (widened >= 0)
            image_release(widened);
    }
}

void expect_idle_pixels(Window* console) {
    const std::vector<uint8_t> drawn = frame_pixels();
    write_picture(drawn, "quickplay-idle.png");
    expect_glass(drawn);
    CHECK(drawn[400 * 640 + 146] == 93);
    // The scanner well's outline, palette 93, round the scanner.
    CHECK(drawn[140 * 640 + 425] == 93 && drawn[140 * 640 + 506] == 93);
    CHECK(drawn[221 * 640 + 425] == 93 && drawn[221 * 640 + 506] == 93);
    CHECK(drawn[180 * 640 + 425] == 93 && drawn[180 * 640 + 506] == 93);
    ScopedServices scope(g_widget);
    const Handle icon = image_load("Widgets.gaf", "Icon_Circle", 1, 9);
    const Handle icon_off = image_load("Widgets.gaf", "Icon_Circle", 2, 9);
    const Handle radio_on = image_load("Widgets.gaf", "Radio_Button", 1, 9);
    const Handle radio_off = image_load("Widgets.gaf", "Radio_Button", 2, 9);
    const Handle checked = image_load("Widgets.gaf", "Check_Box", 4, 9);
    const Handle arm = image_load("Widgets.gaf", "ArmCoreLogos", 0, 9);
    const Handle core = image_load("Widgets.gaf", "ArmCoreLogos", 1, 9);
    const Handle scanner = image_load("BY_New.gaf", "Scanner", 0, 9);
    const Handle large = image_load("Widgets.gaf", "Function_Button_Large", 2, 9);
    const Handle widened = widen_large_button(large);
    expect_picture(drawn, icon, 26, 150, 9, "selected type icon");
    expect_picture(drawn, icon_off, 26, 167, 9, "second type icon");
    expect_picture(drawn, radio_on, 216, 163, 9, "Any planet");
    expect_picture(drawn, radio_off, 300, 163, 9, "Fuel Dump only");
    expect_picture(drawn, radio_off, 216, 183, 9, "Novice");
    expect_picture(drawn, radio_on, 270, 183, 9, "Seasoned");
    expect_picture(drawn, radio_off, 336, 183, 9, "Expert");
    expect_picture(drawn, radio_off, 216, 203, 9, "Arm");
    expect_picture(drawn, radio_off, 266, 203, 9, "Core");
    expect_picture(drawn, radio_on, 320, 203, 9, "Either");
    expect_picture(drawn, arm, 227, 202, 9, "Arm logo");
    expect_picture(drawn, core, 277, 202, 9, "Core logo");
    expect_picture(drawn, checked, 216, 223, 9, "favorite check");
    expect_picture(drawn, scanner, 430, 145, 9, "idle scanner");
    Window* find = named(console, "FIND BATTLE");
    if (find != nullptr)
        expect_caps(drawn, widened, find->x, find->y, "FIND BATTLE");
    bool saw_check = false;
    Surface box{};
    if (checked >= 0 && image_surface(checked, &box) && box.pixels != nullptr) {
        for (int32_t row = 0; row < box.height && !saw_check; ++row) {
            const uint8_t* in = box.pixels + static_cast<std::ptrdiff_t>(row) * box.pitch;
            for (int32_t col = 0; col < box.width; ++col)
                saw_check = saw_check || in[col] == 84;
        }
    }
    CHECK(saw_check);
    const Handle faces[] = {
        icon, icon_off, radio_on, radio_off, checked, arm, core, scanner, large, widened
    };
    for (const Handle face : faces) {
        if (face >= 0)
            image_release(face);
    }
}

void expect_search_pixels(Window* console) {
    const std::vector<uint8_t> drawn = frame_pixels();
    // The log well's outline, palette 90.
    CHECK(drawn[276 * 640 + 158] == 90 && drawn[276 * 640 + 507] == 90);
    CHECK(drawn[379 * 640 + 158] == 90 && drawn[379 * 640 + 507] == 90);
    CHECK(drawn[330 * 640 + 158] == 90 && drawn[330 * 640 + 507] == 90);
    expect_log_glass(drawn);
    ScopedServices scope(g_widget);
    const Handle scanner = image_load("BY_New.gaf", "Scanner", 0, 9);
    const Handle meter = image_load("BY_New.gaf", "Queue_Meter", screens::kQuickPlayMeterFrame, 9);
    const Handle large = image_load("Widgets.gaf", "Function_Button_Large", 2, 9);
    const Handle widened = widen_large_button(large);
    expect_picture(drawn, scanner, 430, 145, 9, "scanner at the start of the search");
    expect_picture(drawn, meter, 300, 259, 9, "queue meter");
    Window* cancel = named(console, "CANCEL SEARCH");
    if (cancel != nullptr)
        expect_caps(drawn, widened, cancel->x, cancel->y, "CANCEL SEARCH");
    const Handle faces[] = {scanner, meter, large, widened};
    for (const Handle face : faces) {
        if (face >= 0)
            image_release(face);
    }
}

std::vector<QuickPlayNotice> g_qp;

ServiceResult record_qp_join(void*, const QuickPlayQuery* query) {
    const char* favorite = "-";
    if (query != nullptr && query->prefer_favorite != 0 && query->favorite[0] != '\0')
        favorite = query->favorite;
    const std::string area =
        query != nullptr && query->area_known ? std::to_string(query->area_id) : "-";
    const int32_t type = query != nullptr ? query->game_type : -1;
    const int32_t where = query != nullptr ? query->where : -1;
    const int32_t skill = query != nullptr ? query->skill : -1;
    const int32_t side = query != nullptr ? query->side : -1;
    g_log.calls.push_back(
        "join type=" + std::to_string(type) + " where=" + std::to_string(where) +
        " skill=" + std::to_string(skill) + " side=" + std::to_string(side) +
        " favorite=" + favorite + " area=" + area
    );
    return ServiceResult::pending;
}

ServiceResult record_qp_cancel(void*) {
    g_log.calls.emplace_back("cancel");
    return ServiceResult::pending;
}

ServiceResult record_qp_answer(void*, const char* offer_id, int32_t accept) {
    g_log.calls.push_back(
        "answer accept=" + std::to_string(accept) + " id=" + (offer_id != nullptr ? offer_id : "")
    );
    return ServiceResult::pending;
}

bool record_qp_take(void*, QuickPlayNotice* out) {
    if (g_qp.empty() || out == nullptr)
        return false;
    *out = g_qp.front();
    g_qp.erase(g_qp.begin());
    return true;
}

ServiceResult record_join_detail(void*, uint32_t area, const char* game_id, const char* password) {
    g_log.calls.push_back(
        "join area=" + std::to_string(area) + " game=" + (game_id != nullptr ? game_id : "") +
        " password=" + (password != nullptr ? password : "-")
    );
    return ServiceResult::pending;
}

ServiceResult record_host_detail(void*, const HostGameRequest* request) {
    const char* name = request != nullptr && request->name != nullptr ? request->name : "";
    const char* password =
        request != nullptr && request->password != nullptr ? request->password : "-";
    const uint32_t area = request != nullptr ? request->area_id : 0;
    const int32_t mode = request != nullptr ? request->game_mode : -1;
    const int32_t skill = request != nullptr ? request->skill : -1;
    const int32_t players = request != nullptr ? request->players : -1;
    const int32_t death = request != nullptr ? request->commander_death : -1;
    g_log.calls.push_back(
        "host area=" + std::to_string(area) + " mode=" + std::to_string(mode) + " name=" + name +
        " password=" + password + " skill=" + std::to_string(skill) +
        " players=" + std::to_string(players) + " death=" + std::to_string(death)
    );
    return ServiceResult::pending;
}

void dismiss_ok() {
    int32_t x = 0;
    int32_t y = 0;
    if (!boneyards_status_widget_message_button(g_widget, message_box_name::ok, &x, &y))
        return;
    boneyards_status_widget_event(g_widget, pointer(MessageCode::left_down, x, y, button_left));
    boneyards_status_widget_event(g_widget, pointer(MessageCode::left_up, x, y, 0));
}

std::string box_text() {
    MessageBox* box = message_box_find_open(&g_widget->desktop);
    Window* text = box != nullptr ? named(&box->base, message_box_name::text) : nullptr;
    return text_of(text);
}

bool logged(const char* text) {
    for (const std::string& call : g_log.calls)
        if (call == text)
            return true;
    return false;
}

void push_status(int32_t result, int32_t queued, int32_t meter, int32_t planets) {
    QuickPlayNotice notice{};
    notice.kind = QuickPlayNoticeKind::status;
    notice.status.result = result;
    notice.status.queued = queued;
    notice.status.meter = meter;
    notice.status.fits = meter;
    notice.status.planets = planets;
    g_qp.push_back(notice);
}

void push_found(const QuickPlayFoundNotice& found) {
    QuickPlayNotice notice{};
    notice.kind = QuickPlayNoticeKind::found;
    notice.found = found;
    g_qp.push_back(notice);
}

void install_quickplay_recorders() {
    g_widget->config.service.quickplay_join = record_qp_join;
    g_widget->config.service.quickplay_cancel = record_qp_cancel;
    g_widget->config.service.quickplay_answer = record_qp_answer;
    g_widget->config.service.quickplay_take = record_qp_take;
    g_widget->config.service.join_game = record_join_detail;
    g_widget->config.service.host_game = record_host_detail;
}

void check_console_protocol(Window* console, screens::QuickPlay* model, Window* offer) {
    install_quickplay_recorders();
    click_window(named(console, "Any planet"));
    click_window(named(console, "Seasoned"));
    click_window(named(console, "Either"));
    click_window(named(console, "Pick0"));
    CHECK(model->prefer_favorite);
    g_log.calls.clear();
    g_qp.clear();
    click_window(named(console, "FIND BATTLE"));
    CHECK(model->searching && model->requests_sent == 1);
    CHECK(logged("join type=1 where=0 skill=1 side=2 favorite=Lava Alley area=2"));

    push_status(0, 1, 1, 25);
    QuickPlayFoundNotice join = join_offer();
    push_found(join);
    advance(console, 0);
    CHECK(model->searching && model->offer_open && offer->visible != 0);
    CHECK(screens::quickplay_meter_frame(model) == 1);
    const std::string offer_text = text_of(named(offer, "Text"));
    CHECK(offer_text.find("Rumble in the Rubble") != std::string::npos);
    CHECK(offer_text.find("Duel on Lava Alley") != std::string::npos);
    CHECK(offer_text.find("Join now?") != std::string::npos);
    const int32_t sent_before_yes = model->requests_sent;
    click_window(named(offer, "Yes"));
    CHECK(model->requests_sent == sent_before_yes + 1);
    CHECK(logged("answer accept=1 id=a1b2c3d4e5f607"));
    CHECK(logged("join area=418 game=c0ffee0a11b223 password=-"));
    // Yes ends the search: back from the battle, the card shows FIND BATTLE.
    CHECK(!model->searching && !model->offer_open && offer->visible == 0);
    const int32_t stopped_at = model->elapsed_ms;
    advance(console, 1000);
    CHECK(!model->searching && model->elapsed_ms == stopped_at);
    expect_visible(console, "FIND BATTLE", true);
    expect_visible(console, "CANCEL SEARCH", false);
    expect_visible(console, "Timer", false);
    // A join the server refused goes on searching, and its status says so.
    push_status(0, 1, 1, 25);
    advance(console, 0);
    CHECK(model->searching);
    expect_visible(console, "CANCEL SEARCH", true);
    expect_visible(console, "FIND BATTLE", false);

    g_log.calls.clear();
    push_found(host_offer(2));
    advance(console, 0);
    CHECK(model->offer_open);
    const std::string host_text = text_of(named(offer, "Text"));
    CHECK(host_text.find("No games available, host one?") != std::string::npos);
    CHECK(host_text.find("ArmSniper") != std::string::npos);
    click_window(named(offer, "Yes"));
    CHECK(logged("answer accept=1 id=a1b2c3d4e5f607"));
    CHECK(logged("host area=2 mode=1 name=ArmSniper password=- skill=1 players=0 death=2"));
    CHECK(!model->searching && !model->offer_open);
    expect_visible(console, "FIND BATTLE", true);
    // Yes sends no cancel: the battle leaves the queue on the server.
    CHECK(!logged("cancel"));
    push_status(0, 1, 1, 25);
    advance(console, 0);
    CHECK(model->searching);

    g_log.calls.clear();
    push_found(join);
    advance(console, 0);
    CHECK(model->offer_open);
    const int32_t elapsed = model->elapsed_ms;
    click_window(named(offer, "No"));
    CHECK(logged("answer accept=0 id=a1b2c3d4e5f607"));
    CHECK(!logged("join area=418 game=c0ffee0a11b223 password=-"));
    CHECK(model->searching && !model->offer_open && model->elapsed_ms == elapsed);
    g_qp.clear();
    push_found(join);
    advance(console, 0);
    CHECK(!model->offer_open);

    g_log.calls.clear();
    push_status(1, 0, 0, 0);
    advance(console, 0);
    CHECK(!model->searching);
    CHECK(box_text().find("QuickPlay is not open for this account.") != std::string::npos);
    CHECK(!logged("cancel"));
    dismiss_ok();

    g_log.calls.clear();
    click_window(named(console, "FIND BATTLE"));
    CHECK(
        model->searching && logged("join type=1 where=0 skill=1 side=2 favorite=Lava Alley area=2")
    );
    advance(console, screens::kQuickPlayStatusWaitMs - 1);
    CHECK(model->searching && message_box_find_open(&g_widget->desktop) == nullptr);
    advance(console, 1);
    CHECK(!model->searching);
    CHECK(box_text().find("The search could not be started.") != std::string::npos);
    CHECK(!logged("cancel"));
    dismiss_ok();

    g_log.calls.clear();
    click_window(named(console, "FIND BATTLE"));
    CHECK(model->searching);
}

void test_console(Data* data) {
    g_log = Log{};
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.palette = &data->palette;
    config.host.play_sound = record_sound;
    config.service.network_available = connected;
    config.service.activate = activate;
    config.service.connected = connected;
    config.service.request_census = record_census;
    config.service.set_events = keep_events;
    config.service.enter_battles = record_enter;
    config.service.leave_battles = record_leave;
    config.service.get_battle = record_get;
    config.service.join_game = record_join;
    config.service.host_game = record_host;
    config.service.send_chat = record_chat;
    config.service.page_user = record_page;
    config.service.locate_user = record_locate;
    config.service.join_room = record_room;
    config.service.create_room = record_create_room;
    config.service.request_profile = record_profile;
    config.service.fetch_file = record_fetch;
    g_widget = boneyards_status_widget_create(config);
    CHECK(g_widget != nullptr);
    if (g_widget == nullptr)
        return;
    click_window(boneyards_status_widget_part(g_widget, OverlayPart::enter));
    MainFrame* frame = g_widget->system.main_frame;
    CHECK(frame != nullptr);
    if (frame == nullptr)
        return;
    for (int32_t i = 0; frame->console_screen != console_screen::login && i < 160; ++i)
        tick(1);
    CHECK(test::load_two_planet_galaxy(&g_widget->navigation->galaxy));
    screens::galaxy_apply_allegiances(&g_widget->navigation->galaxy, "CA");
    {
        ScopedServices scope(g_widget);
        galaxy_panel_sync_hotspots(frame);
        main_frame_close_screen(frame);
    }
    finish_slide(frame);
    click_window(frame->map_panel->hotspots[1]);
    g_widget->navigation->allegiance = 'A';
    g_widget->navigation->profile.profile.favorite_map = nullptr;
    g_widget->system.matchmaking_enabled =
        account_sees_unfinished(kAccountStatusBetaTester) ? 1 : 0;
    main_frame_enable_buttons(frame);
    click_window(frame->quickplay_button);
    finish_slide(frame);
    Window* console = frame->active_screen;
    CHECK(console != nullptr && window_is_named(console, k_quickplay_console_name));
    CHECK(window_is_shown(console));
    CHECK(!boneyards_status_widget_message_open(g_widget));
    CHECK(message_box_find_open(&g_widget->desktop) == nullptr);
    expect_box(
        console,
        main_frame_layout::console_x,
        main_frame_layout::console_y,
        main_frame_layout::console_width,
        main_frame_layout::console_height,
        "QuickPlay"
    );
    CHECK(main_frame_layout::console_x == 23 && main_frame_layout::console_y == 126);
    CHECK(main_frame_layout::console_width == 498 && main_frame_layout::console_height == 313);

    screens::QuickPlay* model = quickplay_console_model(console);
    CHECK(model != nullptr);
    if (model == nullptr)
        return;
    CHECK(g_widget->navigation->galaxy.selected == 1);
    CHECK(model->selected == screens::kQuickPlayDuel);
    CHECK(!model->allowed[screens::kQuickPlayGalacticWar]);
    CHECK(model->allowed[screens::kQuickPlayWarTraining]);
    CHECK(model->requests_sent == 0 && !model->offer_open && !model->searching);
    EXPECT_TEXT(text_of(named(console, "Title")).c_str(), "QUICKPLAY");
    EXPECT_TEXT(text_of(named(console, "Heading")).c_str(), "FIND ME A BATTLE");
    EXPECT_TEXT(text_of(named(console, "Blurb0")).c_str(), "Two commanders, one map,");
    EXPECT_TEXT(text_of(named(console, "Blurb1")).c_str(), "no allies. Ranked on the");
    EXPECT_TEXT(text_of(named(console, "Blurb2")).c_str(), "Duel ladder.");
    // The Duel blurb is drawn whole, left of the rule at x 146.
    for (const char* name : {"Blurb0", "Blurb1", "Blurb2"}) {
        Window* blurb = named(console, name);
        CHECK(blurb != nullptr);
        if (blurb == nullptr)
            continue;
        CHECK(blurb->x == 29 && blurb->x + blurb->width <= 146);
        const std::string shown = text_of(blurb);
        int32_t width = 0;
        int32_t height = 0;
        {
            ScopedServices scope(g_widget);
            text_measure_string(blurb->font, shown.c_str(), &width, &height);
        }
        CHECK(width > 0 && width <= blurb->width);
    }
    EXPECT_TEXT(
        text_of(named(console, "Help0")).c_str(),
        "QuickPlay looks across every planet for a forming battle that fits,"
    );
    EXPECT_TEXT(
        text_of(named(console, "Help3")).c_str(), "ladders count QuickPlay battles like any other."
    );
    EXPECT_TEXT(text_of(named(console, "Favorite")).c_str(), "Prefer my favorite: Lava Alley");
    expect_box(named(console, "Pick0"), 23, 147, 107, 17, "Pick0");
    expect_box(named(console, "Pick3"), 23, 198, 107, 17, "Pick3");
    expect_box(named(console, "Pick4"), 23, 215, 107, 17, "Pick4");
    if (Window* title = named(console, "Title")) {
        expect_at(title, 23, 132, "Title");
        CHECK(title->width == 124);
    }
    if (Window* where = named(console, "Where")) {
        expect_at(where, 150, 162, "Where");
        CHECK(where->width == 60);
    }
    expect_at(named(console, "Any planet"), 216, 162, "Any planet");
    expect_at(named(console, "Fuel Dump only"), 300, 162, "Fuel Dump only");
    expect_at(named(console, "Novice"), 216, 182, "Novice");
    expect_at(named(console, "Seasoned"), 270, 182, "Seasoned");
    expect_at(named(console, "Expert"), 336, 182, "Expert");
    expect_box(named(console, "Arm"), 216, 202, 11, 12, "Arm");
    expect_box(named(console, "Core"), 266, 202, 11, 12, "Core");
    expect_at(named(console, "Either"), 320, 202, "Either");
    expect_at(named(console, "ArmLogo"), 227, 202, "ArmLogo");
    expect_at(named(console, "CoreLogo"), 277, 202, "CoreLogo");
    expect_at(named(console, "ArmName"), 239, 202, "ArmName");
    expect_at(named(console, "CoreName"), 289, 202, "CoreName");
    if (Window* favorite = named(console, "Favorite")) {
        expect_at(favorite, 216, 222, "Favorite");
        CHECK(favorite->height == 11);
        CHECK(favorite->states.current == check_state::checked_up);
    }
    expect_box(named(console, "ScannerWell"), 425, 140, 82, 82, "ScannerWell");
    expect_box(named(console, "Scanner"), 430, 145, 72, 72, "Scanner");
    expect_at(named(console, "Help0"), 160, 300, "Help0");
    expect_box(named(console, "FIND BATTLE"), 200, 262, 160, 21, "FIND BATTLE");
    expect_box(named(console, "CANCEL SEARCH"), 200, 392, 160, 21, "CANCEL SEARCH");
    expect_at(named(console, "SEARCHING"), 160, 258, "SEARCHING");
    expect_at(named(console, "Timer"), 240, 258, "Timer");
    expect_box(named(console, "Queue"), 300, 259, 104, 9, "Queue");
    expect_box(named(console, "LogWell"), 158, 276, 350, 104, "LogWell");
    expect_box(named(console, "Line0"), 162, 279, 340, 14, "Line0");
    expect_at(named(console, "Line4"), 162, 279 + 14 * 4, "Line4");
    expect_visible(console, "FIND BATTLE", true);
    expect_visible(console, "CANCEL SEARCH", false);
    expect_visible(console, "SEARCHING", false);
    expect_visible(console, "Help0", true);
    expect_state(console, "Any planet", button_state::pressed);
    expect_state(console, "Seasoned", button_state::pressed);
    expect_state(console, "Either", button_state::pressed);

    Window* offer = named(console, "QuickPlayFound");
    expect_box(offer, 142, 172, 216, 136, "QuickPlayFound");
    CHECK(offer != nullptr && offer->visible == 0);
    if (offer == nullptr)
        return;
    {
        Window* yes = named(offer, "Yes");
        Window* no = named(offer, "No");
        expect_box(
            yes,
            172,
            278,
            message_box_layout::button_width,
            message_box_layout::button_height,
            "Yes"
        );
        expect_box(
            no, 272, 278, message_box_layout::button_width, message_box_layout::button_height, "No"
        );
        const std::string offer_text = text_of(named(offer, "Text"));
        CHECK(offer_text.find(screens::kQuickPlayOfferLine0) != std::string::npos);
        CHECK(offer_text.find(screens::kQuickPlayOfferLine1) != std::string::npos);
        CHECK(offer_text.find(screens::kQuickPlayOfferLine2) != std::string::npos);
    }

    expect_stretched(named(console, "FIND BATTLE"), "FIND BATTLE");
    expect_stretched(named(console, "CANCEL SEARCH"), "CANCEL SEARCH");
    expect_idle_pixels(console);

    g_widget->config.service.request_allegiance = record_allegiance;
    g_widget->config.service.request_census = record_census;
    g_log.calls.clear();
    g_log.sounds.clear();
    const int32_t sounds_before = heard_count("GREEN_BUTTON");
    click_window(named(console, "Pick3"));
    CHECK(model->selected == screens::kQuickPlayDuel);
    CHECK(heard_count("GREEN_BUTTON") == sounds_before);
    click_window(named(console, "Pick4"));
    CHECK(model->selected == screens::kQuickPlayWarTraining);
    CHECK(heard_count("GREEN_BUTTON") == sounds_before + 1);
    EXPECT_TEXT(text_of(named(console, "Blurb0")).c_str(), "Practice on a planet");
    g_widget->navigation->galaxy.planets[1].side = 'C';
    reread(console);
    CHECK(model->selected == screens::kQuickPlayDuel);
    CHECK(!model->allowed[screens::kQuickPlayWarTraining]);
    g_widget->navigation->galaxy.planets[1].side = 'I';
    reread(console);
    CHECK(model->allowed[screens::kQuickPlayGalacticWar]);
    CHECK(!model->allowed[screens::kQuickPlayWarTraining]);
    click_window(named(console, "Pick3"));
    CHECK(model->selected == screens::kQuickPlayGalacticWar);
    EXPECT_TEXT(text_of(named(console, "Blurb0")).c_str(), "Fight for the planet");
    g_widget->navigation->galaxy.planets[1].side = 'A';
    reread(console);
    CHECK(model->selected == screens::kQuickPlayDuel);
    CHECK(!model->allowed[screens::kQuickPlayGalacticWar]);
    click_window(named(console, "Pick2"));
    EXPECT_TEXT(text_of(named(console, "Blurb0")).c_str(), "A battle that is not");
    click_window(named(console, "Pick0"));
    EXPECT_TEXT(text_of(named(console, "Blurb0")).c_str(), "Two commanders, one map,");

    click_window(named(console, "Favorite"));
    CHECK(!model->prefer_favorite);
    click_window(named(console, "Favorite"));
    CHECK(model->prefer_favorite);
    expect_state(console, "Favorite", check_state::checked_up);

    g_log.calls.clear();
    click_window(named(console, "FIND BATTLE"));
    CHECK(model->searching && model->elapsed_ms == 0);
    CHECK(model->requests_sent == 0 && !model->offer_open);
    CHECK(message_box_find_open(&g_widget->desktop) == nullptr);
    CHECK(offer->visible == 0);
    expect_visible(console, "FIND BATTLE", false);
    expect_visible(console, "CANCEL SEARCH", true);
    expect_visible(console, "SEARCHING", true);
    expect_visible(console, "Help0", false);
    expect_visible(console, "Line0", true);
    expect_visible(console, "Line1", false);
    EXPECT_TEXT(text_of(named(console, "Timer")).c_str(), "0:00");
    EXPECT_TEXT(
        text_of(named(console, "Line0")).c_str(),
        "Scanning 0 planets for a Duel at Seasoned skill..."
    );
    expect_search_pixels(console);

    advance(console, 110);
    CHECK(model->elapsed_ms == 110 && screens::quickplay_scanner_frame(model) == 1);
    {
        ScopedServices scope(g_widget);
        const Handle frame_one = image_load("BY_New.gaf", "Scanner", 1, 9);
        expect_picture(frame_pixels(), frame_one, 430, 145, 9, "scanner frame 1");
        if (frame_one >= 0)
            image_release(frame_one);
    }
    screens::quickplay_find(model);
    advance(console, 0);
    CHECK(model->elapsed_ms == 110);
    advance(console, 110);
    CHECK(screens::quickplay_scanner_frame(model) == 2);
    QuickPlayStatusNotice kept{};
    kept.queued = 1;
    kept.meter = screens::kQuickPlayMeterFrame;
    kept.fits = 6;
    kept.planets = 25;
    screens::quickplay_apply_status(model, &kept);
    store_scan(model, 1, 1, 0, "Altair", "", 0, 0, 0, 0);
    store_scan(model, 2, 2, 0, "Fuel Dump", "Brawl at Dawn", 0, 2, 0, 0);
    store_scan(model, 3, 2, 1, "Deadman Alley", "Last Stand", 4, 0, 0, 0);
    store_scan(model, 4, 3, 0, "Lava Alley", "Rumble in the Rubble", 1, 1, 1, 2);
    advance(console, 7000 - model->elapsed_ms);
    CHECK(model->elapsed_ms == 7000);
    CHECK(screens::quickplay_scanner_frame(model) == 7);
    EXPECT_TEXT(text_of(named(console, "Timer")).c_str(), "0:07");
    CHECK(screens::quickplay_visible_lines(model) == 5);
    expect_visible(console, "Line4", true);
    for (int32_t i = 0; i < 5; ++i) {
        char name[8] = "Line0";
        name[4] = static_cast<char>('0' + i);
        CHECK(text_of(named(console, name)).find("Battle found") == std::string::npos);
    }
    CHECK(model->requests_sent == 0 && !model->offer_open && offer->visible == 0);
    CHECK(message_box_find_open(&g_widget->desktop) == nullptr);
    {
        const std::vector<uint8_t> drawn = frame_pixels();
        write_picture(drawn, "quickplay-searching.png");
        ScopedServices scope(g_widget);
        const Handle frame_seven = image_load("BY_New.gaf", "Scanner", 7, 9);
        expect_picture(drawn, frame_seven, 430, 145, 9, "scanner frame 7");
        if (frame_seven >= 0)
            image_release(frame_seven);
    }

    click_window(named(console, "CANCEL SEARCH"));
    CHECK(!model->searching && model->elapsed_ms == 0);
    expect_visible(console, "FIND BATTLE", true);
    expect_visible(console, "CANCEL SEARCH", false);
    expect_visible(console, "Help0", true);
    expect_visible(console, "SEARCHING", false);
    CHECK(model->where == screens::QuickPlayWhere::any_planet);
    CHECK(model->skill == screens::QuickPlaySkill::seasoned);
    CHECK(model->selected == screens::kQuickPlayDuel);
    CHECK(offer->visible == 0);

    click_window(named(console, "Fuel Dump only"));
    click_window(named(console, "Expert"));
    click_window(named(console, "Pick1"));
    CHECK(model->where == screens::QuickPlayWhere::fuel_dump);
    CHECK(model->skill == screens::QuickPlaySkill::expert);
    CHECK(model->selected == screens::kQuickPlayWinnerTakesAll);
    click_window(named(console, "FIND BATTLE"));
    EXPECT_TEXT(
        text_of(named(console, "Line0")).c_str(),
        "Scanning Fuel Dump for a Winner Takes All at Expert skill..."
    );
    kept = {};
    kept.queued = 1;
    kept.meter = screens::kQuickPlayMeterFrame;
    kept.planets = 1;
    screens::quickplay_apply_status(model, &kept);
    store_scan(model, 1, 2, 0, "Fuel Dump", "Brawl at Dawn", 0, 2, 0, 0);
    advance(console, 7000);
    CHECK(screens::quickplay_visible_lines(model) == 2);
    expect_visible(console, "Line1", true);
    expect_visible(console, "Line2", false);
    EXPECT_TEXT(
        text_of(named(console, "Line1")).c_str(), "Fuel Dump: Brawl at Dawn wants Expert - skipped"
    );
    EXPECT_TEXT(text_of(named(console, "Timer")).c_str(), "0:07");
    CHECK(model->requests_sent == 0 && !model->offer_open && offer->visible == 0);
    CHECK(message_box_find_open(&g_widget->desktop) == nullptr);
    click_window(named(console, "CANCEL SEARCH"));
    CHECK(!model->searching);
    CHECK(model->where == screens::QuickPlayWhere::fuel_dump);
    CHECK(model->skill == screens::QuickPlaySkill::expert);

    if (!g_log.calls.empty()) {
        for (const std::string& call : g_log.calls)
            std::fprintf(stderr, "FAIL search sent %s\n", call.c_str());
        ++g_failures;
    }
    CHECK(model->requests_sent == 0);

    check_console_protocol(console, model, offer);
    boneyards_status_widget_destroy(g_widget);
    g_widget = nullptr;
    CHECK(logged("cancel"));
}

struct ArchiveSource {
    oa::HpiArchive* archive = nullptr;
};

bool read_archive(void* context, const char* path, std::vector<uint8_t>* out) {
    if (context == nullptr || path == nullptr || out == nullptr)
        return false;
    auto* source = static_cast<ArchiveSource*>(context);
    oa::base::bytes::Decoded<std::vector<uint8_t>> read = source->archive->read(path);
    if (!read.ok() || !read.value.has_value())
        return false;
    *out = std::move(*read.value);
    return true;
}

uint8_t* read_toolkit_file(void* user, const char* path, size_t* size) {
    auto* store = static_cast<oa::AssetStore*>(user);
    std::vector<uint8_t> bytes;
    try {
        bytes = store->read(path).bytes;
    } catch (const std::exception&) {
        return nullptr;
    }
    auto* out = static_cast<uint8_t*>(std::malloc(bytes.size() + 1));
    if (out == nullptr)
        return nullptr;
    if (!bytes.empty())
        std::memcpy(out, bytes.data(), bytes.size());
    *size = bytes.size();
    return out;
}

void expect_gui(
    const GuiScreen* screen, const char* name, int32_t x, int32_t y, int32_t width, int32_t height
) {
    const GuiControl* control = gui_screen_find(screen, name);
    if (control == nullptr) {
        std::fprintf(stderr, "FAIL gui missing %s\n", name);
        ++g_failures;
        return;
    }
    if (control->x != x || control->y != y || control->width != width ||
        control->height != height) {
        std::fprintf(
            stderr,
            "FAIL gui %s at %d,%d %dx%d want %d,%d %dx%d\n",
            name,
            control->x,
            control->y,
            control->width,
            control->height,
            x,
            y,
            width,
            height
        );
        ++g_failures;
    }
}

void test_gui_file(Data* data) {
    ToolkitConfig config{};
    config.width = 640;
    config.height = 480;
    config.palette = &data->palette;
    config.files = ToolkitFiles{&data->store, read_toolkit_file};
    Toolkit* toolkit = toolkit_create(&config);
    CHECK(toolkit != nullptr);
    if (toolkit == nullptr)
        return;
    const Services previous = services_install(toolkit_services(toolkit));
    GuiLoadError error{};
    GuiScreen* screen = gui_screen_load("gui/quickplay.gui", &error);
    if (screen == nullptr)
        std::fprintf(stderr, "FAIL gui load: %s\n", error.message);
    CHECK(screen != nullptr);
    if (screen != nullptr) {
        expect_gui(screen, "QuickPlay", 23, 126, 498, 313);
        expect_gui(screen, "Title", 23, 132, 124, 16);
        expect_gui(screen, "Pick0", 23, 147, 107, 17);
        expect_gui(screen, "Pick4", 23, 215, 107, 17);
        expect_gui(screen, "Heading", 160, 136, 160, 16);
        expect_gui(screen, "Where", 150, 162, 60, 14);
        expect_gui(screen, "Map", 150, 222, 60, 14);
        expect_gui(screen, "ScannerWell", 425, 140, 82, 82);
        expect_gui(screen, "Scanner", 430, 145, 72, 72);
        expect_gui(screen, "Help0", 160, 300, 330, 12);
        expect_gui(screen, "FIND BATTLE", 200, 262, 160, 21);
        expect_gui(screen, "SEARCHING", 160, 258, 80, 14);
        expect_gui(screen, "Timer", 240, 258, 40, 14);
        expect_gui(screen, "Queue", 300, 259, 104, 9);
        expect_gui(screen, "LogWell", 158, 276, 350, 104);
        expect_gui(screen, "Line0", 162, 279, 340, 14);
        expect_gui(screen, "Line4", 162, 335, 340, 14);
        expect_gui(screen, "CANCEL SEARCH", 200, 392, 160, 21);
        expect_gui(screen, "Any planet", 216, 162, 80, 14);
        expect_gui(screen, "Favorite", 216, 222, 200, 11);
        expect_gui(screen, "Rule", 146, 129, 1, 307);
        expect_gui(screen, "QuickPlayFound", 142, 172, 216, 136);
        expect_gui(screen, "Yes", 172, 278, 57, 21);
        const GuiControl* find = gui_screen_find(screen, "FIND BATTLE");
        const GuiControl* cancel = gui_screen_find(screen, "CANCEL SEARCH");
        const GuiControl* duel = gui_screen_find(screen, "Blurb0");
        CHECK(find != nullptr && std::strcmp(find->action, "goto:new_quickplay_searching") == 0);
        CHECK(cancel != nullptr && std::strcmp(cancel->action, "goto:new_quickplay") == 0);
        CHECK(duel != nullptr && std::strcmp(duel->caption, "Two commanders, one map,") == 0);
        gui_screen_destroy(screen);
    }
    services_install(previous);
    toolkit_destroy(toolkit);
}

// Both checks read gui/quickplay.gui, so the archive stays installed
// until they finish.
std::vector<std::string> g_launches;

void pump_service(BoneyardsService* online) {
    online->pump(online->user);
}

bool sent_is(std::size_t index, const char* text) {
    ReplaySent sent{};
    const bool found = rover_replay_sent(index, &sent);
    if (!found || std::strcmp(sent.text, text) != 0) {
        std::fprintf(
            stderr,
            "sent %zu is \"%s\", expected \"%s\"\n",
            index,
            found ? sent.text : "(none)",
            text
        );
        return false;
    }
    return true;
}

void check_replay() {
    char error[0x200] = {};
    const bool installed =
        rover_replay_install(OA_BY_CLIENT_TEST_DATA "/quickplay.replay", error, sizeof error);
    if (!installed)
        std::fprintf(stderr, "%s\n", error);
    CHECK(installed);
    if (!installed)
        return;
    *rover_session() = RoverSession{};
    BoneyardsService online = rover_session_service();
    g_launches.clear();
    ServiceEvents events{};
    events.launch = [](void*, const LaunchReport* launch) {
        const char* game_id =
            launch != nullptr && launch->game_id != nullptr ? launch->game_id : "";
        const int32_t creator = launch != nullptr && launch->creator ? 1 : 0;
        const uint32_t area = launch != nullptr ? launch->area_id : 0;
        g_launches.push_back(
            std::string(game_id) + "," + std::to_string(creator) + "," + std::to_string(area)
        );
    };
    online.set_events(online.user, &events);
    if (online.activate(online.user) != ActivationResult::ok) {
        CHECK(false);
        online.set_events(online.user, nullptr);
        rover_replay_uninstall();
        return;
    }
    QuickPlayQuery query{};
    query.game_type = 1;
    query.where = 0;
    query.skill = 1;
    query.side = 2;
    query.prefer_favorite = 1;
    std::snprintf(query.favorite, sizeof query.favorite, "Lava Alley");
    std::size_t at = rover_replay_sent_count();
    CHECK(online.quickplay_join(online.user, &query) == ServiceResult::pending);
    pump_service(&online);
    CHECK(sent_is(
        at,
        "{[GameManager<S:command=quickplay_join><S:favorite=Lava Alley><I:game_type=1>"
        "<I:prefer_favorite=1><I:side=2><I:skill=1><I:where=0>]}"
    ));
    QuickPlayNotice notice{};
    CHECK(online.quickplay_take(online.user, &notice));
    CHECK(notice.kind == QuickPlayNoticeKind::status && notice.status.queued == 1);
    CHECK(notice.status.planets == 25 && notice.status.meter == 1);
    CHECK(online.quickplay_take(online.user, &notice));
    CHECK(notice.kind == QuickPlayNoticeKind::scan && notice.scan.seq == 1);
    CHECK(notice.scan.kind == 1 && std::strcmp(notice.scan.planet, "Altair") == 0);
    CHECK(notice.scan.battle[0] == '\0');
    CHECK(online.quickplay_take(online.user, &notice));
    CHECK(notice.kind == QuickPlayNoticeKind::scan && notice.scan.seq == 2);
    CHECK(notice.scan.kind == 3 && notice.scan.seats == 1 && notice.scan.seats_max == 2);
    CHECK(std::strcmp(notice.scan.battle, "Rumble in the Rubble") == 0);
    screens::QuickPlay card{};
    screens::quickplay_init(&card);
    screens::quickplay_find(&card);
    screens::quickplay_apply_scan(&card, &notice.scan);
    screens::quickplay_advance(&card, 1400);
    char line[screens::kQuickPlayTextCapacity];
    screens::quickplay_scan_line(&card, 1, line, sizeof line);
    EXPECT_TEXT(line, "Lava Alley: Rumble in the Rubble, 1 of 2 seats, Seasoned");
    CHECK(online.quickplay_take(online.user, &notice));
    CHECK(notice.kind == QuickPlayNoticeKind::found && notice.found.role == 0);
    CHECK(notice.found.game_known && notice.found.area_id == 0x1a2);
    CHECK(std::strcmp(notice.found.game_id, "c0ffee0a11b223") == 0);

    at = rover_replay_sent_count();
    CHECK(online.quickplay_answer(online.user, notice.found.offer_id, 1) == ServiceResult::pending);
    pump_service(&online);
    CHECK(sent_is(
        at, "{[GameManager<I:accept=1><S:command=quickplay_answer><S:offer_id=a1b2c3d4e5f607>]}"
    ));
    CHECK(online.quickplay_take(online.user, &notice));
    CHECK(notice.kind == QuickPlayNoticeKind::status && notice.status.queued == 1);
    at = rover_replay_sent_count();
    CHECK(
        online.join_game(online.user, 0x1a2, "c0ffee0a11b223", nullptr) == ServiceResult::pending
    );
    pump_service(&online);
    CHECK(
        sent_is(at, "{[GameManager<U:area_id=1a2><S:command=join_game><S:game_id=c0ffee0a11b223>]}")
    );
    // <U:area_id=1a2> is hexadecimal: the battle's planet is 418.
    CHECK(g_launches.size() == 1 && g_launches[0] == "c0ffee0a11b223,0,418");

    CHECK(online.quickplay_join(online.user, &query) == ServiceResult::pending);
    pump_service(&online);
    CHECK(online.quickplay_take(online.user, &notice));
    CHECK(notice.kind == QuickPlayNoticeKind::found && notice.found.role == 1);
    CHECK(notice.found.seats == 2 && notice.found.on_death == 2 && notice.found.skill == 1);
    screens::quickplay_apply_found(&card, &notice.found);
    char offer[192];
    screens::quickplay_offer_line(&card, offer, sizeof offer);
    CHECK(std::strcmp(offer, "ArmSniper\nDuel on Empyrrean\nNo games available, host one?") == 0);
    at = rover_replay_sent_count();
    CHECK(online.quickplay_answer(online.user, notice.found.offer_id, 1) == ServiceResult::pending);
    pump_service(&online);
    CHECK(sent_is(
        at, "{[GameManager<I:accept=1><S:command=quickplay_answer><S:offer_id=a1b2c3d4e5f607>]}"
    ));
    CHECK(online.quickplay_take(online.user, &notice));
    CHECK(notice.kind == QuickPlayNoticeKind::status && notice.status.queued == 1);
    HostGameRequest host{};
    CHECK(screens::quickplay_host_request(&card, &host));
    at = rover_replay_sent_count();
    CHECK(online.host_game(online.user, &host) == ServiceResult::pending);
    pump_service(&online);
    CHECK(sent_is(
        at,
        "{[GameManager<U:area_id=2><S:command=host_game><S:name=ArmSniper><I:on_death=2>"
        "<I:players=2><I:skill=1><I:type=1>]}"
    ));
    CHECK(g_launches.size() == 2 && g_launches[1] == "c0ffee0a11b223,1,2");

    CHECK(online.quickplay_join(online.user, &query) == ServiceResult::pending);
    pump_service(&online);
    CHECK(online.quickplay_take(online.user, &notice));
    CHECK(notice.found.role == 0 && std::strcmp(notice.found.offer_id, "bbbbbbbbbbbbbb") == 0);
    at = rover_replay_sent_count();
    CHECK(online.quickplay_answer(online.user, notice.found.offer_id, 0) == ServiceResult::pending);
    pump_service(&online);
    CHECK(sent_is(
        at, "{[GameManager<I:accept=0><S:command=quickplay_answer><S:offer_id=bbbbbbbbbbbbbb>]}"
    ));
    CHECK(online.quickplay_take(online.user, &notice));
    CHECK(notice.kind == QuickPlayNoticeKind::status && notice.status.queued == 1);

    at = rover_replay_sent_count();
    CHECK(online.quickplay_cancel(online.user) == ServiceResult::pending);
    pump_service(&online);
    CHECK(sent_is(at, "{[GameManager<S:command=quickplay_cancel>]}"));
    CHECK(online.quickplay_take(online.user, &notice));
    CHECK(notice.kind == QuickPlayNoticeKind::status && notice.status.queued == 0);

    query.game_type = 4;
    query.prefer_favorite = 0;
    query.favorite[0] = '\0';
    query.area_id = 0x1a2;
    query.area_known = true;
    at = rover_replay_sent_count();
    CHECK(online.quickplay_join(online.user, &query) == ServiceResult::pending);
    pump_service(&online);
    CHECK(sent_is(
        at,
        "{[GameManager<U:area_id=1a2><S:command=quickplay_join><I:game_type=4>"
        "<I:prefer_favorite=0><I:side=2><I:skill=1><I:where=0>]}"
    ));
    CHECK(online.quickplay_take(online.user, &notice));
    CHECK(notice.kind == QuickPlayNoticeKind::status && notice.status.planets == 1);
    CHECK(!online.quickplay_take(online.user, &notice));

    online.close(online.user);
    pump_service(&online);
    pump_service(&online);
    online.set_events(online.user, nullptr);
    rover_replay_uninstall();
}

void test_from_archive(Data* data, oa::HpiArchive* archive) {
    ArchiveSource source;
    source.archive = archive;
    rover_session_set_virtual_files(read_archive, &source);
    test_gui_file(data);
    test_console(data);
    check_replay();
    rover_session_set_virtual_files(nullptr, nullptr);
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the QuickPlay console tests");
    test_model();
    test_search_records();
    const std::filesystem::path coreprime = oa::boneyards_test::coreprime_archive(argc, argv);
    if (coreprime.empty()) {
        std::fprintf(stderr, "the mod folder is required\n");
        return 1;
    }
    oa::base::bytes::Decoded<oa::HpiArchive> opened = oa::open_hpi_file(coreprime);
    if (!opened.ok() || !opened.value.has_value()) {
        std::fprintf(stderr, "coreprime.ccx could not be opened\n");
        return 1;
    }
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0)
            g_frames = std::filesystem::path(argv[i + 1]);
    }
    // The game's files are the install's alone, as on a profile that does not
    // mount the mod: the layouts and the new art come through the client
    // session's reader, from the archive that travels with the program.
    Data data;
    (void)data.store.discover("31");
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int i = 0; i < OA_PALETTE_COLORS; ++i)
        data.palette.entries[i] = oa::PaletteEntry{raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};
    test_from_archive(&data, &*opened.value);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("quickplay tests passed\n");
    return 0;
}
