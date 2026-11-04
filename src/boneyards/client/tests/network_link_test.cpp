// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Enter Boneyards asks whether any interface other than the loopback is up.
// A supplied list stands in for the machine: loopback alone shows the
// connect message and leaves the login unopened; another interface that is
// up continues into the login console.
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/network_link.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/formats/hpi.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace oa::boneyards;
using namespace oa::boneyards::console;

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

NetworkInterface iface(bool up, bool loopback) {
    NetworkInterface item{};
    item.up = up;
    item.loopback = loopback;
    return item;
}

struct ListedInterfaces {
    NetworkInterface items[8]{};
    std::size_t count = 0;
};

std::size_t supply_interfaces(void* context, NetworkInterface* out, std::size_t capacity) {
    const auto* listed = static_cast<const ListedInterfaces*>(context);
    std::size_t count = listed->count;
    if (count > capacity)
        count = capacity;
    for (std::size_t i = 0; i < count; ++i)
        out[i] = listed->items[i];
    return count;
}

// Reports a huge count after writing one loopback entry, to check the limit.
std::size_t overlong_loopback(void*, NetworkInterface* out, std::size_t capacity) {
    if (out != nullptr && capacity > 0)
        out[0] = iface(true, true);
    return static_cast<std::size_t>(-1);
}

bool session_network(void*) {
    const BoneyardsService service = rover_session_service();
    return service.network_available(service.user);
}

void test_supplied_list() {
    check(!network_link_present(nullptr, 4), "a missing list is no link");
    const NetworkInterface loopback_only[] = {iface(true, true), iface(true, true)};
    check(!network_link_present(loopback_only, 2), "loopback, up or not, is no link");
    const NetworkInterface down[] = {iface(true, true), iface(false, false)};
    check(!network_link_present(down, 2), "an interface that is not up is no link");
    const NetworkInterface linked[] = {iface(true, true), iface(true, false)};
    check(network_link_present(linked, 2), "an up interface other than loopback is a link");

    ListedInterfaces listed{};
    listed.items[0] = iface(true, true);
    listed.count = 1;
    rover_session_set_network_interfaces(supply_interfaces, &listed);
    check(!session_network(nullptr), "the session reports loopback only as no network");

    listed.items[1] = iface(false, false);
    listed.count = 2;
    check(!session_network(nullptr), "a down interface beside loopback is still no network");

    listed.items[1] = iface(true, false);
    listed.count = 2;
    check(session_network(nullptr), "the session reports another up interface as a network");

    rover_session_set_network_interfaces(overlong_loopback, nullptr);
    check(!network_link_available(), "a count past the buffer is not read off its end");

    rover_session_set_network_interfaces(nullptr, nullptr);
    (void)network_link_available();
}

struct Gate {
    int32_t logins = 0;
    int32_t activations = 0;
    int32_t state = -1;
    std::vector<std::string> messages;
};

struct Assets {
    oa::AssetStore store;
    oa::Palette palette{};
    Gate gate{};

    explicit Assets(const std::filesystem::path& root) : store(root) {
        store.mount(root / "by.ccx");
    }
};

bool read_asset(void* context, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<Assets*>(context)->store.read(path).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void host_message(void* context, const char* text) {
    static_cast<Assets*>(context)->gate.messages.emplace_back(text != nullptr ? text : "");
}

void host_state(void* context, uint8_t state) {
    static_cast<Assets*>(context)->gate.state = state;
}

ActivationResult counting_activate(void* context) {
    ++static_cast<Assets*>(context)->gate.activations;
    return ActivationResult::ok;
}

ServiceResult counting_login(void* context, const LoginRequest*) {
    ++static_cast<Assets*>(context)->gate.logins;
    return ServiceResult::pending;
}

Message pointer(MessageCode code, int32_t x, int32_t y, uint32_t buttons) {
    Message msg{};
    msg.code = code;
    msg.x = x;
    msg.y = y;
    msg.buttons = buttons;
    return msg;
}

void click_enter(StatusWidget* widget) {
    const Window* enter = boneyards_status_widget_part(widget, OverlayPart::enter);
    const int32_t x = enter->x + enter->width / 2;
    const int32_t y = enter->y + enter->height / 2;
    boneyards_status_widget_event(widget, pointer(MessageCode::left_down, x, y, button_left));
    boneyards_status_widget_event(widget, pointer(MessageCode::left_up, x, y, 0));
}

StatusWidget* open_menu(Assets* assets) {
    assets->gate = Gate{};
    StatusWidgetConfig config{};
    config.files = FileReader{assets, read_asset};
    config.palette = &assets->palette;
    config.host.user = assets;
    config.host.show_message_box = host_message;
    config.host.set_frontend_state = host_state;
    config.service.user = assets;
    config.service.network_available = session_network;
    config.service.activate = counting_activate;
    config.service.login = counting_login;
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr, "main menu overlay created");
    return widget;
}

void test_enter(Assets* assets, ListedInterfaces* listed, bool linked) {
    rover_session_set_network_interfaces(supply_interfaces, listed);
    StatusWidget* widget = open_menu(assets);
    if (widget == nullptr)
        return;
    click_enter(widget);
    if (!linked) {
        check(
            assets->gate.messages.size() == 1 &&
                assets->gate.messages[0] == menu_message::internet_required,
            "loopback only asks to connect to the Internet"
        );
        check(assets->gate.logins == 0, "loopback only does not start a login");
        check(assets->gate.activations == 0, "loopback only does not start the client");
        check(
            assets->gate.state != frontend_value::state_boneyards &&
                boneyards_status_widget_visible(widget) &&
                !boneyards_status_widget_in_boneyards(widget),
            "loopback only stays on the main menu"
        );
        const Window* enter = boneyards_status_widget_part(widget, OverlayPart::enter);
        check(
            enter->states.current == button_state::disabled,
            "the connect message disables Enter Boneyards"
        );
    } else {
        check(assets->gate.messages.empty(), "an up interface shows no connect message");
        check(assets->gate.activations == 1, "an up interface starts the client");
        check(
            assets->gate.state == frontend_value::state_boneyards &&
                boneyards_status_widget_in_boneyards(widget),
            "an up interface enters the Boneyards"
        );
        for (int32_t i = 0; i < 180; ++i)
            boneyards_status_widget_tick(widget);
        MainFrameProbe probe{};
        check(
            boneyards_status_widget_probe(widget, &probe) &&
                probe.console_screen == console_screen::login,
            "an up interface opens the login console"
        );
        check(assets->gate.logins == 0, "the login console waits for the player to log in");
    }
    boneyards_status_widget_destroy(widget);
}

void test_menu(const std::filesystem::path& root) {
    Assets assets(root);
    ListedInterfaces loopback{};
    loopback.items[0] = iface(true, true);
    loopback.items[1] = iface(false, false);
    loopback.count = 2;
    test_enter(&assets, &loopback, false);

    ListedInterfaces linked{};
    linked.items[0] = iface(true, true);
    linked.items[1] = iface(true, false);
    linked.count = 2;
    test_enter(&assets, &linked, true);

    rover_session_set_network_interfaces(nullptr, nullptr);
}

} // namespace

int main() {
    const std::filesystem::path root = oa::boneyards_test::require_by_install("the network check");
    test_supplied_list();
    test_menu(root);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
