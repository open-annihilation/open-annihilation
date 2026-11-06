// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The real update screen. Each case parses a catalogue or a feed, or hashes a
// file, and the lines are what that check found. Nothing is fetched.
#include "console_state.hpp"

#include "oa/base/sha256.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
#include "oa/boneyards/screens/update_pass.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/data/engine_feed/feed.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {

namespace catalogue = oa::data::catalogue;
namespace feed = oa::data::engine_feed;
namespace sha = oa::base::sha256;

struct Fixture {
    oa::AssetStore assets{oa::boneyards_test::by_install()};
    oa::Palette palette{};
};

bool read_asset(void* context, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<Fixture*>(context)->assets.read(path).bytes;
        return true;
    } catch (...) {
        return false;
    }
}

void click(StatusWidget* widget, Window* window) {
    if (window == nullptr)
        return;
    Message message{};
    message.x = window->x + window->width / 2;
    message.y = window->y + window->height / 2;
    message.code = MessageCode::left_down;
    message.buttons = button_left;
    boneyards_status_widget_event(widget, message);
    message.code = MessageCode::left_up;
    message.buttons = 0;
    boneyards_status_widget_event(widget, message);
}

Window* at(Window* parent, int32_t x, int32_t y) {
    for (int32_t i = 0; i < parent->children.count; ++i) {
        Window* child = parent->children.items[i];
        if (child->x == parent->x + x && child->y == parent->y + y)
            return child;
    }
    return nullptr;
}

const char* status_text(screens::ByListBox* list, int32_t index) {
    auto* row = reinterpret_cast<screens::ByTextRow*>(list_box_item(&list->list, index));
    if (row == nullptr || row->label == nullptr)
        return "";
    return row->label->ops->state_text(row->label, -1);
}

std::string lines_of(StatusWidget* widget, screens::ByListBox* list) {
    ScopedServices scope(widget);
    std::string text;
    for (int32_t i = 0; i < list->list.count; ++i) {
        if (i != 0)
            text.push_back('\n');
        text += status_text(list, i);
    }
    return text;
}

int32_t bar_fill_rows(StatusWidget* widget, Window* bar) {
    ScopedServices scope(widget);
    Surface track{};
    if (bar == nullptr || !image_surface(bar->states.images[1], &track) || track.height < 3)
        return -1;
    int32_t rows = 0;
    for (int32_t y = track.height - 2; y >= 1; --y) {
        if (track.pixels[y * track.pitch + 1] == screens::kWidgetKey)
            break;
        ++rows;
    }
    return rows;
}

struct Held {
    StatusWidget* widget{nullptr};

    ~Held() { boneyards_status_widget_destroy(widget); }
};

StatusWidget* make_widget(Fixture* fixture) {
    StatusWidgetConfig config{};
    config.files = {fixture, read_asset};
    config.palette = &fixture->palette;
    config.service.user = fixture;
    config.service.network_available = [](void*) { return true; };
    config.service.activate = [](void*) { return ActivationResult::ok; };
    return boneyards_status_widget_create(config);
}

/// Opens the update console with a picture set before the screen is built.
///
/// @param fixture the install the console reads
/// @param facts the checks this case performed
/// @return the widget, on the update console, or null
StatusWidget* open_update(Fixture* fixture, const UpdateCheckFacts& facts) {
    StatusWidget* widget = make_widget(fixture);
    OA_CHECK(widget != nullptr);
    if (widget == nullptr)
        return nullptr;
    boneyards_status_widget_begin_real_update(widget);
    boneyards_status_widget_set_real_update(widget, boneyards_update_picture(facts));
    click(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    for (int32_t i = 0; i < 80; ++i) {
        boneyards_status_widget_tick(widget);
        const MainFrame* frame = widget->system.main_frame;
        if (frame != nullptr && frame->console_screen == console_screen::update &&
            frame->active_screen != nullptr && frame->active_screen->visible != 0)
            return widget;
    }
    OA_CHECK(false);
    return widget;
}

std::string joined(StatusWidget* widget) {
    Window* view = widget->system.main_frame->active_screen;
    auto* status = reinterpret_cast<screens::ByListBox*>(at(view, 0x6f, 0x32));
    OA_CHECK(status != nullptr);
    if (status == nullptr)
        return {};
    return lines_of(widget, status);
}

bool contains(std::string_view text, std::string_view needle) {
    // The status list wraps a sentence across rows and drops the break's
    // space. Joining those rows with a space puts the sentence back.
    std::string flat;
    flat.reserve(text.size());
    bool pending_space = false;
    for (const char ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
            pending_space = !flat.empty();
            continue;
        }
        if (pending_space) {
            flat.push_back(' ');
            pending_space = false;
        }
        flat.push_back(ch);
    }
    return flat.find(needle) != std::string::npos;
}

std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

std::string hex_of(std::string_view body) {
    const sha::Digest digest = sha::digest_of(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(body.data()), body.size())
    );
    const auto hex = sha::to_hex(digest);
    return {hex.data(), hex.size()};
}

void write_bytes(const std::filesystem::path& path, std::string_view body) {
    std::ofstream out(path, std::ios::binary);
    OA_CHECK(static_cast<bool>(out));
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    OA_CHECK(static_cast<bool>(out));
}

sha::Digest hash_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    OA_CHECK(static_cast<bool>(in));
    const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return sha::digest_of(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(body.data()), body.size())
    );
}

std::optional<catalogue::Catalogue> read_expansion(
    std::string_view id, std::string_view version, std::string_view hash, std::string_view size
) {
    std::string json = "{\"catalogue\":1,\"registry\":\"coreprime\",\"sequence\":7,"
                       "\"generated\":\"2026-11-02T04:10:00Z\","
                       "\"expires\":\"2026-12-02T04:10:00Z\",\"packages\":[{"
                       "\"kind\":\"expansion\",\"id\":\"";
    json += id;
    json += "\",\"name\":\"Yard pack\",\"version\":\"";
    json += version;
    json += "\",\"release\":2,\"size\":";
    json += size;
    json += ",\"sha256\":\"";
    json += hash;
    json += "\",\"file\":\"/v1/p/";
    json += hash;
    json += ".oaexp\",\"extends\":[\"totala\"]}]}";
    const std::vector<uint8_t> bytes = bytes_of(json);
    catalogue::Catalogue out;
    std::string error;
    const bool read = catalogue::read_catalogue(bytes, out, &error);
    OA_CHECK(read);
    OA_CHECK(out.packages.size() == 1);
    if (!read || out.packages.size() != 1)
        return std::nullopt;
    return out;
}

std::optional<feed::Feed> read_one_feed(std::string_view release, std::string_view version) {
    const std::string hash = hex_of("feed-package");
    std::string json = "{\"feed\":1,\"channel\":\"stable\",\"releases\":[{\"release\":";
    json += release;
    json += ",\"version\":\"";
    json += version;
    json += "\",\"date\":\"2026-10-24\",\"from\":\"0.7.0\",\"packages\":{\"macos-universal\":{"
            "\"file\":\"oa.zip\",\"size\":4,\"sha256\":\"";
    json += hash;
    json += "\",\"mirrors\":[\"http://downloads.example.net/v1/engine/";
    json += version;
    json += "/\"]}}}]}";
    const std::vector<uint8_t> bytes = bytes_of(json);
    std::string error;
    std::optional<feed::Feed> out = feed::read_feed(bytes, &error);
    OA_CHECK(out.has_value());
    return out;
}

feed::Build build_at(std::string_view version, int64_t release) {
    feed::Build build;
    const std::optional<oa::formats::oamod::EngineVersion> parsed =
        oa::formats::oamod::parse_engine_version(version);
    OA_CHECK(parsed.has_value());
    if (parsed)
        build.version = *parsed;
    build.release = release;
    build.package = "macos-universal";
    return build;
}

void tick_n(StatusWidget* widget, int32_t count) {
    for (int32_t i = 0; i < count; ++i)
        boneyards_status_widget_tick(widget);
}

void test_services_off(Fixture* fixture) {
    UpdateCheckFacts facts;
    facts.services_checked = true;
    facts.services_on = false;
    Held held;
    held.widget = open_update(fixture, facts);
    if (held.widget == nullptr || held.widget->system.main_frame == nullptr)
        return;
    const std::string lines = joined(held.widget);
    OA_CHECK(contains(lines, "Online services are off. Turn them on from the Settings page."));
    OA_CHECK(!contains(lines, "Game files"));
    OA_CHECK(!contains(lines, "Open Annihilation"));
    OA_CHECK(!contains(lines, "Boneyards expansion"));
    OA_CHECK(!contains(lines, "Checking auto-updates"));
    Window* bar = at(held.widget->system.main_frame->active_screen, 0x190, 0x32);
    OA_CHECK(bar_fill_rows(held.widget, bar) == 0);
    tick_n(held.widget, 20);
    OA_CHECK(held.widget->system.main_frame->console_screen == console_screen::update);
}

void test_bad_hash(Fixture* fixture, const std::filesystem::path& scratch) {
    constexpr std::string_view body = "good";
    const std::filesystem::path file = scratch / "fixture.hpi";
    write_bytes(file, body);
    const sha::Digest hashed = hash_file(file);
    const std::string wrong = hex_of("other");
    const std::optional<catalogue::Catalogue> parsed = read_expansion("boneyards", "2", wrong, "4");
    if (!parsed || parsed->packages.empty())
        return;
    OA_CHECK(hashed != parsed->packages[0].sha256);
    UpdateCheckFacts facts;
    facts.services_checked = true;
    facts.services_on = true;
    facts.files_checked = true;
    facts.mismatched_file = file.filename().string();
    Held held;
    held.widget = open_update(fixture, facts);
    if (held.widget == nullptr || held.widget->system.main_frame == nullptr)
        return;
    const std::string lines = joined(held.widget);
    OA_CHECK(contains(lines, "fixture.hpi"));
    OA_CHECK(contains(lines, "Restore it from the Settings page."));
    OA_CHECK(!contains(lines, "Open Annihilation"));
    OA_CHECK(!contains(lines, "up to date"));
    OA_CHECK(!contains(lines, "Checking auto-updates"));
    tick_n(held.widget, 20);
    OA_CHECK(held.widget->system.main_frame->console_screen == console_screen::update);
}

void test_update_offered(Fixture* fixture) {
    const std::optional<feed::Feed> parsed = read_one_feed("901", "0.9.1");
    if (!parsed || parsed->releases.empty())
        return;
    const feed::Offer offer = feed::choose(*parsed, build_at("0.8.0", 800));
    OA_CHECK(offer.way != feed::Way::none);
    OA_CHECK(offer.release != nullptr);
    if (offer.release == nullptr)
        return;
    UpdateCheckFacts facts;
    facts.services_checked = true;
    facts.services_on = true;
    facts.files_checked = true;
    facts.feed_checked = true;
    facts.update_offered = true;
    facts.offered_version = offer.release->version_text;
    Held held;
    held.widget = open_update(fixture, facts);
    if (held.widget == nullptr || held.widget->system.main_frame == nullptr)
        return;
    const std::string lines = joined(held.widget);
    OA_CHECK(contains(lines, "available"));
    OA_CHECK(contains(lines, offer.release->version_text));
    OA_CHECK(!contains(lines, "Boneyards expansion"));
    OA_CHECK(!contains(lines, "Checking auto-updates"));
    tick_n(held.widget, 20);
    OA_CHECK(held.widget->system.main_frame->console_screen == console_screen::update);
}

void expect_login_after_hold(StatusWidget* widget) {
    OA_CHECK(widget->system.main_frame->console_screen == console_screen::update);
    boneyards_status_widget_tick(widget);
    OA_CHECK(widget->system.main_frame->console_screen == console_screen::update);
    boneyards_status_widget_tick(widget);
    OA_CHECK(widget->system.main_frame->console_screen == console_screen::login);
}

void test_extension_installed(Fixture* fixture, const std::filesystem::path& scratch) {
    constexpr std::string_view body = "yard";
    const std::filesystem::path file = scratch / "yard.oaexp";
    write_bytes(file, body);
    const sha::Digest hashed = hash_file(file);
    const std::string hash = hex_of(body);
    const std::optional<catalogue::Catalogue> parsed = read_expansion("boneyards", "4", hash, "4");
    if (!parsed || parsed->packages.empty())
        return;
    OA_CHECK(hashed == parsed->packages[0].sha256);
    const std::optional<feed::Feed> newer = read_one_feed("100", "0.1.0");
    if (!newer)
        return;
    const feed::Offer offer = feed::choose(*newer, build_at("0.8.0", 800));
    OA_CHECK(offer.way == feed::Way::none);
    UpdateCheckFacts facts;
    facts.services_checked = true;
    facts.services_on = true;
    facts.files_checked = true;
    facts.feed_checked = true;
    facts.extension_checked = true;
    facts.extension_installed_now = true;
    facts.extension_version = parsed->packages[0].version;
    Held held;
    held.widget = open_update(fixture, facts);
    if (held.widget == nullptr || held.widget->system.main_frame == nullptr)
        return;
    const std::string lines = joined(held.widget);
    OA_CHECK(contains(lines, "Installed the Boneyards expansion"));
    OA_CHECK(contains(lines, parsed->packages[0].version));
    OA_CHECK(!contains(lines, "Checking auto-updates"));
    Window* bar = at(held.widget->system.main_frame->active_screen, 0x190, 0x32);
    OA_CHECK(bar_fill_rows(held.widget, bar) == 0xbb - 2);
    expect_login_after_hold(held.widget);
}

void test_all_good(Fixture* fixture, const std::filesystem::path& scratch) {
    constexpr std::string_view body = "same";
    const std::filesystem::path file = scratch / "same.oaexp";
    write_bytes(file, body);
    const sha::Digest hashed = hash_file(file);
    const std::string hash = hex_of(body);
    const std::optional<catalogue::Catalogue> parsed = read_expansion("boneyards", "5", hash, "4");
    if (!parsed || parsed->packages.empty())
        return;
    OA_CHECK(hashed == parsed->packages[0].sha256);
    const std::optional<feed::Feed> current = read_one_feed("100", "0.1.0");
    if (!current)
        return;
    const feed::Offer offer = feed::choose(*current, build_at("0.8.0", 800));
    OA_CHECK(offer.way == feed::Way::none);
    UpdateCheckFacts facts;
    facts.services_checked = true;
    facts.services_on = true;
    facts.files_checked = true;
    facts.feed_checked = true;
    facts.extension_checked = true;
    facts.extension_current = true;
    facts.extension_version = parsed->packages[0].version;
    Held held;
    held.widget = open_update(fixture, facts);
    if (held.widget == nullptr || held.widget->system.main_frame == nullptr)
        return;
    const std::string lines = joined(held.widget);
    OA_CHECK(contains(lines, "Online services are on."));
    OA_CHECK(contains(lines, "Game files match."));
    OA_CHECK(contains(lines, "Open Annihilation is up to date."));
    OA_CHECK(contains(lines, "The Boneyards expansion is up to date"));
    OA_CHECK(contains(lines, parsed->packages[0].version));
    OA_CHECK(!contains(lines, "Checking auto-updates"));
    expect_login_after_hold(held.widget);
}

} // namespace

int main() {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the update screen checks");
    Fixture fixture;
    fixture.assets.mount(root / "by.ccx");
    fixture.assets.mount(root / "totala1.hpi");
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int32_t i = 0; i < 256; ++i)
        fixture.palette.entries[i] = {
            raw[static_cast<std::size_t>(i) * 4],
            raw[static_cast<std::size_t>(i) * 4 + 1],
            raw[static_cast<std::size_t>(i) * 4 + 2],
            0
        };
    const std::filesystem::path scratch = oa::test::make_scratch_directory("F24-update-screen");
    test_services_off(&fixture);
    test_bad_hash(&fixture, scratch);
    test_update_offered(&fixture);
    test_extension_installed(&fixture, scratch);
    test_all_good(&fixture, scratch);
    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    return oa::test::check_exit_status();
}
