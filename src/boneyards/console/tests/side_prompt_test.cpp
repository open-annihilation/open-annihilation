// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The side prompt and the Allegiance tab. A replay dossier with no side, a
// previous side and an open lock is read. The login prompt is shown for an
// unset member, preselects the previous side, sends ALLEGIANCE and opens the
// Main Frame only after the refreshed dossier names a side. A set side and a
// guest skip it. The prompt opens on Choose and Tab stays inside it.
// Escape and EXIT leave. The tab stays
// enabled while ALLEGIANCE_LOCKED is 0 and shows a disabled side at 1.
// In English and each language the Boneyards' words are translated into,
// the box is the message-box picture's size, centred on the console, every
// control and picture lies inside it and each of the three messages fits
// its room. The console around the box shows only the console's green.
// With --frames DIR after the mod folder, the prompt is written there in
// each language as side-prompt-TAG.png.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "console_layer.hpp"
#include "console_state.hpp"
#include "oa/boneyards/rover_replay.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/chat.hpp"
#include "oa/boneyards/screens/login.hpp"
#include "oa/boneyards/screens/profile.hpp"
#include "oa/boneyards/shown_text.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/png.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::console;
using namespace oa::boneyards::screens;

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

struct FakeServer {
    bool connected = false;
    int32_t logins = 0;
    int32_t pumps = 0;
    ServiceEvents events{};
    void (*on_pump)() = nullptr;
};

FakeServer g_server;

std::vector<std::string> g_sounds;

void host_sound(void*, const char* name) {
    g_sounds.emplace_back(name);
}

bool heard(const char* name) {
    for (const std::string& sound : g_sounds)
        if (sound == name)
            return true;
    return false;
}

bool fake_network(void*) {
    return true;
}

ActivationResult fake_activate(void*) {
    return ActivationResult::ok;
}

bool fake_connected(void*) {
    return g_server.connected;
}

ServiceResult fake_pending_login(void*, const LoginRequest*) {
    ++g_server.logins;
    return ServiceResult::pending;
}

void fake_set_events(void*, const ServiceEvents* events) {
    g_server.events = events != nullptr ? *events : ServiceEvents{};
}

void fake_pump(void*) {
    ++g_server.pumps;
    void (*action)() = g_server.on_pump;
    g_server.on_pump = nullptr;
    if (action != nullptr)
        action();
}

struct ProfileSend {
    int calls = 0;
    uint32_t user_id = 0;
    std::string key;
    std::string value;
    ProfileUpdatedCallback updated = nullptr;
    void* context = nullptr;
};

struct ProfileAsk {
    int calls = 0;
    uint32_t user_id = 0;
    ProfileCallback on_profile = nullptr;
    void* context = nullptr;
};

ProfileSend g_sent;
ProfileAsk g_asked;

ServiceResult fake_submit_profile(
    void*,
    uint32_t user_id,
    const ProfileEdits* edits,
    ProfileUpdatedCallback updated,
    void* context
) {
    ++g_sent.calls;
    g_sent.user_id = user_id;
    g_sent.updated = updated;
    g_sent.context = context;
    g_sent.key.clear();
    g_sent.value.clear();
    if (edits != nullptr && edits->count > 0) {
        g_sent.key = edits->fields[0].key != nullptr ? edits->fields[0].key : "";
        g_sent.value = edits->fields[0].value;
    }
    return ServiceResult::pending;
}

ServiceResult fake_request_profile(
    void*, uint32_t user_id, ProfileCallback on_profile, LadderCallback, void* context
) {
    ++g_asked.calls;
    g_asked.user_id = user_id;
    g_asked.on_profile = on_profile;
    g_asked.context = context;
    return ServiceResult::pending;
}

struct DossierPlan {
    char allegiance = 'U';
    char previous = '\0';
    uint32_t user_id = 42;
};

DossierPlan g_plan;

void deliver_dossier() {
    if (g_server.events.session_changed == nullptr)
        return;
    g_server.connected = true;
    SessionRecord session{};
    session.level = SessionLevel::logged_in;
    session.user_id = g_plan.user_id;
    session.area_id = 7;
    session.allegiance = g_plan.allegiance;
    session.allegiance_previous = g_plan.previous;
    session.allegiance_locked = 0;
    g_server.events.session_changed(g_server.events.user, &session);
    session.level = SessionLevel::dossier_loaded;
    g_server.events.session_changed(g_server.events.user, &session);
}

Message pointer(MessageCode code, int32_t x, int32_t y, uint32_t buttons) {
    Message msg{};
    msg.code = code;
    msg.x = x;
    msg.y = y;
    msg.buttons = buttons;
    return msg;
}

void click_window(StatusWidget* widget, const Window* window) {
    const int32_t x = window->x + window->width / 2;
    const int32_t y = window->y + window->height / 2;
    boneyards_status_widget_event(widget, pointer(MessageCode::left_down, x, y, button_left));
    boneyards_status_widget_event(widget, pointer(MessageCode::left_up, x, y, 0));
}

void press(StatusWidget* widget, int32_t k) {
    Message msg{};
    msg.code = MessageCode::key_down;
    msg.key = k;
    boneyards_status_widget_event(widget, msg);
    msg.code = MessageCode::key_up;
    boneyards_status_widget_event(widget, msg);
}

void type(StatusWidget* widget, const char* text) {
    for (; *text != '\0'; ++text)
        press(widget, *text);
}

StatusWidget* make_widget(Data* data) {
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.palette = &data->palette;
    config.host.play_sound = host_sound;
    config.service.network_available = fake_network;
    config.service.activate = fake_activate;
    config.service.connected = fake_connected;
    config.service.login = fake_pending_login;
    config.service.set_events = fake_set_events;
    config.service.pump = fake_pump;
    config.service.submit_profile = fake_submit_profile;
    config.service.request_profile = fake_request_profile;
    return boneyards_status_widget_create(config);
}

void finish_slide(StatusWidget* widget) {
    const ConsoleLayer* layer = widget->system.main_frame->console;
    for (int32_t i = 0; layer->sliding != 0 && i < 100; ++i)
        boneyards_status_widget_tick(widget);
}

MainFrame* enter_boneyards(StatusWidget* widget) {
    click_window(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    for (int32_t i = 0; i < 16; ++i)
        boneyards_status_widget_tick(widget);
    if (widget->system.main_frame != nullptr)
        finish_slide(widget);
    return widget->system.main_frame;
}

void finish_update_console(StatusWidget* widget) {
    MainFrame* frame = widget->system.main_frame;
    for (int32_t i = 0; frame->console_screen != console_screen::login && i < 160; ++i)
        boneyards_status_widget_tick(widget);
}

void submit_member(StatusWidget* widget, MainFrame* frame) {
    if (frame->active_screen == nullptr)
        return;
    type(widget, "commander");
    Window* member = window_find_named(frame->active_screen, "Registered User", nullptr);
    if (member != nullptr)
        click_window(widget, member);
    Window* password = window_find_named(frame->active_screen, "Password", nullptr);
    if (password != nullptr) {
        ScopedServices scope(widget);
        desktop_set_focus(&widget->desktop, password);
    }
    type(widget, "secret");
    boneyards_status_widget_tick(widget);
    press(widget, key::enter);
}

void submit_guest(StatusWidget* widget, MainFrame* frame) {
    if (frame->active_screen == nullptr || widget->navigation == nullptr)
        return;
    type(widget, "commander");
    // Guest Login is drawn disabled. The check selects it on the dialog.
    login_on_control(&widget->navigation->login, LoginControl::guest_radio);
    boneyards_status_widget_tick(widget);
    Window* login = window_find_named(frame->active_screen, "LOGIN", nullptr);
    if (login != nullptr)
        click_window(widget, login);
}

struct BroughtUp {
    StatusWidget* widget = nullptr;
    MainFrame* frame = nullptr;
};

void release_widget(BroughtUp* opened) {
    if (opened->widget != nullptr)
        boneyards_status_widget_destroy(opened->widget);
    opened->widget = nullptr;
    opened->frame = nullptr;
}

BroughtUp login_as(Data* data, char allegiance, char previous, uint32_t user_id, bool guest) {
    g_server = FakeServer{};
    g_sounds.clear();
    g_sent = ProfileSend{};
    g_asked = ProfileAsk{};
    g_plan = DossierPlan{allegiance, previous, user_id};
    BroughtUp opened;
    opened.widget = make_widget(data);
    if (opened.widget == nullptr)
        return opened;
    opened.frame = enter_boneyards(opened.widget);
    if (opened.frame == nullptr)
        return opened;
    finish_update_console(opened.widget);
    if (guest)
        submit_guest(opened.widget, opened.frame);
    else
        submit_member(opened.widget, opened.frame);
    g_server.on_pump = deliver_dossier;
    boneyards_status_widget_tick(opened.widget);
    return opened;
}

Window* side_prompt_of(MainFrame* frame) {
    return frame != nullptr ? window_find_named(&frame->base, "SidePrompt", nullptr) : nullptr;
}

// A caption's text lives in the widget's own text table: read it with the
// widget's services installed.
std::string caption_of(StatusWidget* widget, Window* window) {
    if (window == nullptr || window->ops == nullptr || window->ops->state_text == nullptr)
        return {};
    ScopedServices scope(widget);
    const char* caption = window->ops->state_text(window, -1);
    return caption != nullptr ? caption : "";
}

bool login_closed(const MainFrame* frame) {
    return frame->console_screen == console_screen::none && frame->active_screen == nullptr &&
           heard("GREEN_SLIDE");
}

void answer_profile(const char* allegiance) {
    struct Bag {
        std::map<std::string, std::string> strings;
    };

    static Bag bag;
    bag.strings["ALLEGIANCE"] = allegiance;
    auto read = [](void* user, const char* key, char* out, std::size_t capacity) {
        const auto& strings = static_cast<const Bag*>(user)->strings;
        const auto found = strings.find(key);
        if (found == strings.end())
            return false;
        std::snprintf(out, capacity, "%s", found->second.c_str());
        return true;
    };
    ProfileFields fields{};
    fields.user = &bag;
    fields.get_string = read;
    g_asked.on_profile(g_asked.context, true, &fields);
}

void test_replay_dossier() {
    char error[0x200] = {};
    const bool installed =
        rover_replay_install(OA_BY_CLIENT_TEST_DATA "/side_prompt.replay", error, sizeof error);
    check(installed, "the side prompt replay installs");
    if (!installed)
        return;
    *rover_session() = RoverSession{};
    BoneyardsService service = rover_session_service();
    const bool activated = service.activate(service.user) == ActivationResult::ok;
    check(activated, "the replay client activates");
    if (activated) {
        LoginRequest request{};
        request.user_name = "SideCommander";
        request.password = "";
        request.register_new = false;
        check(
            service.login(service.user, &request) == ServiceResult::pending,
            "the replay login is pending"
        );
        for (int step = 0;
             step < 20 && rover_session()->record.level != SessionLevel::dossier_loaded;
             ++step)
            service.pump(service.user);
        const SessionRecord& record = rover_session()->record;
        check(record.level == SessionLevel::dossier_loaded, "the replay dossier loads");
        check(record.user_id == 7, "the replay user id");
        check(record.allegiance == 'U', "a dossier with no ALLEGIANCE is unset");
        check(record.allegiance_previous == 'C', "ALLEGIANCE_PREVIOUS CORE is read");
        check(record.allegiance_locked == 0, "ALLEGIANCE_LOCKED 0 is read");
        check(std::strcmp(record.user_name, "SideCommander") == 0, "the replay login name");
        if (service.close != nullptr)
            service.close(service.user);
        service.pump(service.user);
        service.set_events(service.user, nullptr);
    }
    rover_replay_uninstall();
    *rover_session() = RoverSession{};
}

bool prompt_ready(const BroughtUp& opened, const char* what) {
    if (opened.widget == nullptr || opened.frame == nullptr) {
        check(false, what);
        return false;
    }
    return true;
}

void test_prompt_for_previous_core(Data* data) {
    BroughtUp opened = login_as(data, 'U', 'C', 42, false);
    if (!prompt_ready(opened, "a member with no side reaches the dossier")) {
        release_widget(&opened);
        return;
    }
    Window* prompt = side_prompt_of(opened.frame);
    check(
        prompt != nullptr && window_is_shown(prompt) &&
            opened.frame->console_screen == console_screen::login,
        "an unset member stays on the login with the side prompt"
    );
    if (prompt == nullptr) {
        release_widget(&opened);
        return;
    }
    Window* arm = window_find_named(prompt, "Arm", nullptr);
    Window* core = window_find_named(prompt, "Core", nullptr);
    const std::string words =
        caption_of(opened.widget, window_find_named(prompt, "Message", nullptr));
    check(
        arm != nullptr && core != nullptr && arm->states.current == button_state::unpressed &&
            core->states.current == button_state::pressed,
        "the last sector's Core side starts selected"
    );
    check(
        words.find("Core") != std::string::npos && words.find("last sector") != std::string::npos,
        "the prompt names the last sector's Core side"
    );
    Window* choose_first = window_find_named(prompt, "Choose", nullptr);
    check(opened.widget->desktop.focus == prompt, "the prompt keeps the keyboard on its root");
    press(opened.widget, key::tab);
    check(opened.widget->desktop.focus == choose_first, "the first Tab lands on Choose");
    bool inside = false;
    for (Window* cursor = opened.widget->desktop.focus; cursor != nullptr; cursor = cursor->parent)
        inside = inside || cursor == prompt;
    check(inside, "Tab stays inside the side prompt");

    Window* cancel = window_find_named(opened.frame->active_screen, "CANCEL", nullptr);
    check(cancel != nullptr, "the login CANCEL button");
    if (cancel != nullptr) {
        const int32_t x = cancel->x + cancel->width / 2;
        const int32_t y = cancel->y + cancel->height / 2;
        Window* hit = window_hit_test(&opened.frame->base, x, y);
        check(hit != nullptr && hit != cancel, "the prompt catches a click over CANCEL");
    }
    boneyards_status_widget_event(
        opened.widget, pointer(MessageCode::left_down, 10, 10, button_left)
    );
    boneyards_status_widget_event(opened.widget, pointer(MessageCode::left_up, 10, 10, 0));
    check(
        opened.widget->navigation->login.state == LoginState::planetary &&
            side_prompt_of(opened.frame) != nullptr,
        "a click that is not a choice leaves the prompt up"
    );

    Window* choose = window_find_named(prompt, "Choose", nullptr);
    check(choose != nullptr, "the Choose button");
    if (choose != nullptr) {
        click_window(opened.widget, choose);
        click_window(opened.widget, choose);
    }
    check(
        g_sent.calls == 1 && g_sent.user_id == 42 && g_sent.key == "ALLEGIANCE" &&
            g_sent.value == "CORE",
        "Choose sends ALLEGIANCE CORE once while the answer is outstanding"
    );
    check(g_sent.updated != nullptr, "the profile answer is waited on");
    if (g_sent.updated != nullptr)
        g_sent.updated(g_sent.context, false);
    check(
        opened.widget->navigation->allegiance == 'U' && side_prompt_of(opened.frame) != nullptr,
        "a refused choice stays on the prompt"
    );

    if (choose != nullptr)
        click_window(opened.widget, choose);
    check(g_sent.calls == 2 && g_sent.value == "CORE", "Choose sends the side again");
    if (g_sent.updated != nullptr)
        g_sent.updated(g_sent.context, true);
    check(
        g_asked.calls == 1 && g_asked.user_id == 42 && g_asked.on_profile != nullptr,
        "a stored side asks for the refreshed dossier"
    );
    if (g_asked.on_profile != nullptr)
        answer_profile("U");
    boneyards_status_widget_tick(opened.widget);
    check(
        opened.widget->navigation->allegiance == 'U' &&
            opened.frame->console_screen == console_screen::login &&
            side_prompt_of(opened.frame) != nullptr,
        "a refreshed dossier that is still unset keeps the prompt"
    );

    g_sounds.clear();
    if (g_asked.on_profile != nullptr)
        answer_profile("CORE");
    check(opened.widget->navigation->allegiance == 'C', "the refreshed dossier names Core");
    boneyards_status_widget_tick(opened.widget);
    check(
        login_closed(opened.frame) && side_prompt_of(opened.frame) == nullptr,
        "the Main Frame opens after the refreshed dossier names a side"
    );
    release_widget(&opened);
}

void test_prompt_for_previous_arm(Data* data) {
    BroughtUp opened = login_as(data, 'U', 'A', 42, false);
    if (!prompt_ready(opened, "a member whose last side was Arm reaches the dossier")) {
        release_widget(&opened);
        return;
    }
    Window* prompt = side_prompt_of(opened.frame);
    Window* arm = prompt != nullptr ? window_find_named(prompt, "Arm", nullptr) : nullptr;
    Window* core = prompt != nullptr ? window_find_named(prompt, "Core", nullptr) : nullptr;
    const std::string words = caption_of(
        opened.widget, prompt != nullptr ? window_find_named(prompt, "Message", nullptr) : nullptr
    );
    check(
        prompt != nullptr && arm != nullptr && core != nullptr &&
            arm->states.current == button_state::pressed &&
            core->states.current == button_state::unpressed &&
            words.find("Arm") != std::string::npos &&
            words.find("last sector") != std::string::npos,
        "the last sector's Arm side is named and selected"
    );
    press(opened.widget, key::escape);
    boneyards_status_widget_tick(opened.widget);
    check(opened.widget->system.main_frame == nullptr, "Escape closes the side prompt");
    opened.frame = nullptr;
    release_widget(&opened);
}

void test_no_prompt(
    Data* data, char allegiance, char previous, uint32_t user_id, bool guest, const char* what
) {
    BroughtUp opened = login_as(data, allegiance, previous, user_id, guest);
    if (!prompt_ready(opened, what)) {
        release_widget(&opened);
        return;
    }
    check(side_prompt_of(opened.frame) == nullptr && login_closed(opened.frame), what);
    release_widget(&opened);
}

void test_choice_waits_for_a_radio(Data* data) {
    BroughtUp opened = login_as(data, 'U', '\0', 42, false);
    if (!prompt_ready(opened, "a member with no previous side reaches the dossier")) {
        release_widget(&opened);
        return;
    }
    Window* prompt = side_prompt_of(opened.frame);
    Window* arm = prompt != nullptr ? window_find_named(prompt, "Arm", nullptr) : nullptr;
    Window* core = prompt != nullptr ? window_find_named(prompt, "Core", nullptr) : nullptr;
    Window* choose = prompt != nullptr ? window_find_named(prompt, "Choose", nullptr) : nullptr;
    const std::string words = caption_of(
        opened.widget, prompt != nullptr ? window_find_named(prompt, "Message", nullptr) : nullptr
    );
    check(
        prompt != nullptr && arm != nullptr && core != nullptr && choose != nullptr &&
            arm->states.current == button_state::unpressed &&
            core->states.current == button_state::unpressed &&
            words.find("this sector") != std::string::npos &&
            words.find("last sector") == std::string::npos,
        "no previous side leaves both choices clear"
    );
    if (choose != nullptr)
        click_window(opened.widget, choose);
    check(g_sent.calls == 0, "Choose with no side selected sends nothing");
    if (core != nullptr)
        click_window(opened.widget, core);
    if (choose != nullptr)
        click_window(opened.widget, choose);
    check(
        core != nullptr && core->states.current == button_state::pressed && g_sent.calls == 1 &&
            g_sent.key == "ALLEGIANCE" && g_sent.value == "CORE",
        "Choose after Core is selected sends CORE"
    );
    release_widget(&opened);
}

void test_exit_leaves(Data* data) {
    BroughtUp opened = login_as(data, 'U', 'C', 42, false);
    if (!prompt_ready(opened, "EXIT starts from the side prompt")) {
        release_widget(&opened);
        return;
    }
    Window* prompt = side_prompt_of(opened.frame);
    Window* leave = prompt != nullptr ? window_find_named(prompt, "EXIT", nullptr) : nullptr;
    check(leave != nullptr, "the EXIT button");
    if (leave != nullptr)
        click_window(opened.widget, leave);
    boneyards_status_widget_tick(opened.widget);
    check(
        opened.widget->system.main_frame == nullptr &&
            boneyards_status_widget_visible(opened.widget),
        "EXIT leaves the Boneyards"
    );
    opened.frame = nullptr;
    release_widget(&opened);
}

// The Boneyards' words in each language they are translated into, read from
// the source tree.
oa::data::languages::InterfaceText g_catalogue;

// The folder --frames names, where the prompt is written as a picture in
// each language. Empty writes none.
std::filesystem::path g_frames;

// English, then each language the Boneyards' words are translated into.
constexpr const char* kLanguageTags[] = {"en", "de", "fr", "it", "es", "zh-Hans"};

// What the prompt says with no side last sector, after Arm and after Core.
constexpr const char* kPromptMessages[] = {
    "Choose Arm or Core for this sector.",
    "You chose Arm in the last sector. Choose a side for this one.",
    "You chose Core in the last sector. Choose a side for this one.",
};

// The login console's controls, which the prompt covers.
constexpr const char* kLoginControls[] = {
    "Username",
    "Password",
    "Registered User",
    "Guest Login",
    "REGISTER",
    "LOGIN",
    "CANCEL",
    "Server List",
    "Status",
};

constexpr int32_t kScreenWidth = 640;
constexpr int32_t kScreenHeight = 480;

bool read_catalogue() {
    std::ifstream in(OA_BONEYARDS_CATALOGUE, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::string error;
    if (text.empty() || !g_catalogue.add(text, &error)) {
        std::fprintf(stderr, "the Boneyards catalogue was not read: %s\n", error.c_str());
        return false;
    }
    return true;
}

void use_language(const char* tag) {
    const oa::data::languages::Language* language = oa::data::languages::find_by_tag(tag);
    check(language != nullptr, "the language is known");
    if (language != nullptr)
        oa::data::languages::set_interface_language(&g_catalogue, *language);
}

// A 640x480 frame the widget draws into.
struct Screen {
    std::vector<uint8_t> pixels{};
    Surface surface{};

    Screen() : pixels(static_cast<std::size_t>(kScreenWidth) * kScreenHeight, 0) {
        surface.width = surface.pitch = kScreenWidth;
        surface.height = kScreenHeight;
        surface.pixels = pixels.data();
        surface.clip = {0, 0, kScreenWidth - 1, kScreenHeight - 1};
    }

    uint8_t at(int32_t x, int32_t y) const {
        if (x < 0 || y < 0 || x >= kScreenWidth || y >= kScreenHeight)
            return 0;
        return pixels[static_cast<std::size_t>(y) * kScreenWidth + static_cast<std::size_t>(x)];
    }
};

// Writes a drawn frame as an indexed PNG in the widget's palette.
void write_picture(const StatusWidget* widget, const Screen& screen, const std::string& name) {
    if (g_frames.empty() || widget == nullptr || widget->config.palette == nullptr)
        return;
    std::vector<oa::formats::png::Rgb> colors(static_cast<std::size_t>(OA_PALETTE_COLORS));
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i) {
        const oa::PaletteEntry& entry = widget->config.palette->entries[i];
        colors[static_cast<std::size_t>(i)] = oa::formats::png::Rgb{entry.r, entry.g, entry.b};
    }
    oa::formats::png::Image image;
    image.header.width = kScreenWidth;
    image.header.height = kScreenHeight;
    image.header.bit_depth = 8;
    image.header.color_type = oa::formats::png::ColorType::palette;
    image.palette = colors;
    image.rows = screen.pixels;
    std::vector<uint8_t> bytes;
    check(oa::formats::png::write(image, &bytes), "the picture is encoded");
    std::error_code error;
    std::filesystem::create_directories(g_frames, error);
    std::ofstream out(g_frames / name, std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    check(out.good(), "the picture is written");
}

bool lies_inside(const Window& inner, const Window& outer) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

bool held_by(const Window* window, const Window* ancestor) {
    for (; window != nullptr; window = window->parent)
        if (window == ancestor)
            return true;
    return false;
}

const char* name_of(const Window& window) {
    return window.name != nullptr ? window.name : "(unnamed)";
}

// The picture a window shows in its current state, or k_no_handle.
Handle shown_picture(const Window& window) {
    const int32_t state = window.states.current;
    if (window.states.images == nullptr || state < 0 || state >= window.states.count)
        return k_no_handle;
    return window.states.images[state];
}

// How a message breaks in a font: its lines, the widest, their height with
// any ink past the font's line, and whether one word is wider than the room.
struct WrappedText {
    int32_t lines = 0;
    int32_t widest = 0;
    int32_t height = 0;
    bool word_wide = false;
};

WrappedText wrap_text(StatusWidget* widget, Handle font, const std::string& text, int32_t room) {
    WrappedText wrapped;
    ScopedServices scope(widget);
    Toolkit* toolkit = widget->toolkit;
    if (toolkit == nullptr)
        return wrapped;
    std::vector<char> broken(text.size() * 2 + 2);
    wrapped.lines = toolkit_wrap_text(
        toolkit, room, text.c_str(), broken.data(), static_cast<int32_t>(broken.size()), font
    );
    const std::string lines(broken.data());
    std::size_t start = 0;
    while (start < lines.size()) {
        std::size_t end = lines.find('\n', start);
        if (end == std::string::npos)
            end = lines.size();
        const std::string line = lines.substr(start, end - start);
        TextFit fit{};
        toolkit_text_fit(toolkit, font, line.c_str(), &fit);
        if (fit.width > wrapped.widest)
            wrapped.widest = fit.width;
        wrapped.height += fit.line_height + fit.above + fit.below;
        start = end + 1;
    }
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t space = text.find(' ', at);
        if (space == std::string::npos)
            space = text.size();
        const std::string word = text.substr(at, space - at);
        int32_t width = 0;
        int32_t height = 0;
        if (!word.empty())
            toolkit_measure_text(toolkit, font, word.c_str(), &width, &height);
        if (width > room)
            wrapped.word_wide = true;
        at = space + 1;
    }
    return wrapped;
}

void expect_message_fits(
    StatusWidget* widget, const Window& message, const std::string& text, const char* tag
) {
    const WrappedText wrapped = wrap_text(widget, message.font, text, message.width);
    if (wrapped.lines < 1 || wrapped.word_wide || wrapped.widest > message.width ||
        wrapped.height > message.height) {
        std::fprintf(
            stderr,
            "FAIL: %s \"%s\" takes %d lines, %d wide and %d high, in a room %dx%d%s\n",
            tag,
            text.c_str(),
            wrapped.lines,
            wrapped.widest,
            wrapped.height,
            message.width,
            message.height,
            wrapped.word_wide ? ", a word wider than the room" : ""
        );
        ++g_failures;
    }
}

// The box is the message-box picture at its own size, centred on the
// console, and every control of the prompt but the cover, with the picture
// it shows, lies inside it. A radio's face is drawn a pixel below its top.
void expect_inside_box(
    StatusWidget* widget,
    const Window& prompt,
    const Window& face,
    const Window* cover,
    const Window& area,
    const char* tag
) {
    int32_t art_width = 0;
    int32_t art_height = 0;
    {
        ScopedServices scope(widget);
        image_size(shown_picture(face), &art_width, &art_height);
    }
    if (art_width != face.width || art_height != face.height) {
        std::fprintf(
            stderr,
            "FAIL: %s the box is %dx%d, its picture %dx%d\n",
            tag,
            face.width,
            face.height,
            art_width,
            art_height
        );
        ++g_failures;
    }
    check(
        art_width > 0 && art_height > 0 &&
            std::abs(2 * face.x + face.width - (2 * area.x + area.width)) <= 1 &&
            std::abs(2 * face.y + face.height - (2 * area.y + area.height)) <= 1,
        "the box is centred on the console"
    );
    int32_t controls = 0;
    for (int32_t i = 0; i < prompt.children.count; ++i) {
        const Window* control = prompt.children.items[i];
        if (control == nullptr || control == cover || control == &face)
            continue;
        ++controls;
        int32_t width = 0;
        int32_t height = 0;
        const Handle picture = shown_picture(*control);
        if (picture >= 0) {
            ScopedServices scope(widget);
            image_size(picture, &width, &height);
        }
        const bool radio = control->ops != nullptr && control->ops->is_class != nullptr &&
                           control->ops->is_class(class_name::radio_button);
        const int32_t drop = radio ? 1 : 0;
        if (!lies_inside(*control, face) || width > control->width ||
            height + drop > control->height) {
            std::fprintf(
                stderr,
                "FAIL: %s %s at %d,%d %dx%d with a %dx%d picture lies outside the box "
                "%d,%d %dx%d\n",
                tag,
                name_of(*control),
                control->x,
                control->y,
                control->width,
                control->height,
                width,
                height,
                face.x,
                face.y,
                face.width,
                face.height
            );
            ++g_failures;
        }
    }
    check(controls == 7, "the message, two marks, two sides and two buttons are in the box");
}

// The cover fills the console and shows the console's own picture: no
// control of the login console is drawn in `screen` or takes a click while
// the prompt is up.
void expect_login_covered(
    StatusWidget* widget,
    MainFrame* frame,
    const Screen& screen,
    const Window& prompt,
    const Window& face,
    const Window& cover,
    const char* tag
) {
    const Window& layer = frame->console->base;
    check(
        cover.x == layer.x && cover.y == layer.y && cover.width == layer.width &&
            cover.height == layer.height,
        "the cover fills the console"
    );
    const Handle green = shown_picture(layer);
    check(green >= 0 && shown_picture(cover) == green, "the cover shows the console's picture");
    for (const char* name : kLoginControls) {
        const Window* control = frame->active_screen != nullptr
                                    ? window_find_named(frame->active_screen, name, nullptr)
                                    : nullptr;
        check(control != nullptr, "a login control under the prompt");
        if (control == nullptr)
            continue;
        const Window* hit = window_hit_test(
            &frame->base, control->x + control->width / 2, control->y + control->height / 2
        );
        if (!held_by(hit, &prompt)) {
            std::fprintf(stderr, "FAIL: %s a click on %s reaches the login\n", tag, name);
            ++g_failures;
        }
    }
    Surface picture{};
    bool readable = false;
    {
        ScopedServices scope(widget);
        readable = image_surface(green, &picture) && picture.pixels != nullptr;
    }
    check(readable, "the console's picture is readable");
    if (!readable)
        return;
    int32_t differing = 0;
    for (int32_t y = layer.y; y < layer.y + layer.height && y - layer.y < picture.height; ++y) {
        for (int32_t x = layer.x; x < layer.x + layer.width && x - layer.x < picture.width; ++x) {
            if (x >= face.x && x < face.x + face.width && y >= face.y && y < face.y + face.height)
                continue;
            const uint8_t want =
                picture.pixels
                    [static_cast<std::ptrdiff_t>(y - layer.y) * picture.pitch + (x - layer.x)];
            if (want != resource::console_key && screen.at(x, y) != want)
                ++differing;
        }
    }
    if (differing != 0) {
        std::fprintf(
            stderr,
            "FAIL: %s %d pixels of the console around the box are not the console's\n",
            tag,
            differing
        );
        ++g_failures;
    }
}

void test_layout_in(Data* data, const char* tag) {
    use_language(tag);
    BroughtUp opened = login_as(data, 'U', 'C', 42, false);
    if (!prompt_ready(opened, "the prompt opens in each language")) {
        release_widget(&opened);
        return;
    }
    Window* prompt = side_prompt_of(opened.frame);
    Screen screen;
    boneyards_status_widget_draw(opened.widget, &screen.surface);
    write_picture(opened.widget, screen, std::string("side-prompt-") + tag + ".png");
    Window* face = prompt != nullptr ? window_find_named(prompt, "Face", nullptr) : nullptr;
    Window* cover = prompt != nullptr ? window_find_named(prompt, "Cover", nullptr) : nullptr;
    Window* message = prompt != nullptr ? window_find_named(prompt, "Message", nullptr) : nullptr;
    check(
        face != nullptr && message != nullptr && opened.frame->console != nullptr,
        "the prompt has its box and message over the console"
    );
    check(cover != nullptr, "the prompt covers the console");
    if (face == nullptr || message == nullptr || opened.frame->console == nullptr) {
        release_widget(&opened);
        return;
    }
    expect_inside_box(opened.widget, *prompt, *face, cover, opened.frame->console->base, tag);
    const std::string shown = shown_text(kPromptMessages[2]);
    check(caption_of(opened.widget, message) == shown, "the prompt says the Core message");
    for (const char* english : kPromptMessages)
        expect_message_fits(opened.widget, *message, shown_text(english), tag);
    if (cover != nullptr)
        expect_login_covered(opened.widget, opened.frame, screen, *prompt, *face, *cover, tag);
    release_widget(&opened);
}

struct FieldBag {
    std::map<std::string, std::string> strings;
    std::map<std::string, int32_t> ints;
};

bool bag_string(void* user, const char* key, char* out, std::size_t capacity) {
    const auto& strings = static_cast<const FieldBag*>(user)->strings;
    const auto found = strings.find(key);
    if (found == strings.end())
        return false;
    std::snprintf(out, capacity, "%s", found->second.c_str());
    return true;
}

bool bag_int(void* user, const char* key, int32_t* out) {
    const auto& ints = static_cast<const FieldBag*>(user)->ints;
    const auto found = ints.find(key);
    if (found == ints.end())
        return false;
    *out = found->second;
    return true;
}

bool bag_uint(void* user, const char* key, uint32_t* out) {
    const auto& strings = static_cast<const FieldBag*>(user)->strings;
    const auto found = strings.find(key);
    if (found == strings.end())
        return false;
    *out = static_cast<uint32_t>(std::strtoul(found->second.c_str(), nullptr, 10));
    return true;
}

ProfileFields fields_of(FieldBag* bag) {
    ProfileFields fields{};
    fields.user = bag;
    fields.get_string = bag_string;
    fields.get_int = bag_int;
    fields.get_uint = bag_uint;
    return fields;
}

FieldBag arm_reply(bool with_lock, int32_t lock) {
    FieldBag bag;
    bag.strings["user_id"] = "12";
    bag.strings["LOGINNAME"] = "SideCommander";
    bag.strings["ALLEGIANCE"] = "ARM";
    if (with_lock)
        bag.ints["ALLEGIANCE_LOCKED"] = lock;
    return bag;
}

ProfileSend g_tab_send;

ServiceResult
tab_submit(void*, uint32_t user_id, const ProfileEdits* edits, ProfileUpdatedCallback, void*) {
    ++g_tab_send.calls;
    g_tab_send.user_id = user_id;
    g_tab_send.key.clear();
    g_tab_send.value.clear();
    if (edits != nullptr && edits->count > 0) {
        g_tab_send.key = edits->fields[0].key != nullptr ? edits->fields[0].key : "";
        g_tab_send.value = edits->fields[0].value;
    }
    return ServiceResult::pending;
}

ServiceResult tab_request(void*, uint32_t, ProfileCallback, LadderCallback, void*) {
    return ServiceResult::pending;
}

void receive_arm(ProfileConsole* console, bool with_lock, int32_t lock) {
    FieldBag bag = arm_reply(with_lock, lock);
    ProfileFields fields = fields_of(&bag);
    profile_on_received(console, true, &fields);
}

const int32_t kAllegianceTab = static_cast<int32_t>(ProfileTab::allegiance);

void expect_open_arm(const ProfileConsole& console, const char* what) {
    check(
        console.profile.allegiance == 'A' && console.profile.allegiance_locked == 0 &&
            console.tabs.radios_shown && console.tabs.logo_shown && console.tabs.side_shown &&
            console.tabs.arm_value == widget_value::selected &&
            console.tabs.core_value == widget_value::enabled &&
            std::strcmp(console.tabs.side_name, "Arm") == 0,
        what
    );
}

void test_allegiance_tab() {
    ChatSession session{};
    session.self_user_id = 42;
    BoneyardsService service = boneyards_offline_service(nullptr);
    service.submit_profile = tab_submit;
    service.request_profile = tab_request;
    static ProfileConsole console;
    profile_console_init(&console, &service, &session, SettingsStore{}, nullptr, nullptr, nullptr);

    profile_select_tab(&console, kAllegianceTab);
    receive_arm(&console, false, 0);
    check(
        console.profile.allegiance_locked == -1 && !console.tabs.radios_shown &&
            console.tabs.logo_shown && console.tabs.side_shown &&
            std::strcmp(console.tabs.side_name, "Arm") == 0,
        "a reply that omits the lock hides the choices once a side is set"
    );

    receive_arm(&console, true, 0);
    expect_open_arm(console, "lock 0 keeps the choices enabled after the reply refreshes the tab");

    console.tabs.arm_value = widget_value::enabled;
    console.tabs.core_value = widget_value::selected;
    profile_check_tab(&console, kAllegianceTab);
    g_tab_send = ProfileSend{};
    profile_apply(&console);
    check(
        console.tab_dirty[kAllegianceTab] && g_tab_send.calls == 1 && g_tab_send.user_id == 42 &&
            g_tab_send.key == "ALLEGIANCE" && g_tab_send.value == "CORE",
        "an open tab sends ALLEGIANCE when the side changes"
    );

    console.tabs.arm_value = widget_value::selected;
    console.tabs.core_value = widget_value::enabled;
    profile_check_tab(&console, kAllegianceTab);
    g_tab_send = ProfileSend{};
    profile_apply(&console);
    check(
        !console.tab_dirty[kAllegianceTab] && g_tab_send.calls == 0,
        "choosing the side the record already has sends nothing"
    );

    receive_arm(&console, true, 1);
    profile_check_tab(&console, kAllegianceTab);
    g_tab_send = ProfileSend{};
    profile_apply(&console);
    check(
        console.profile.allegiance_locked == 1 && console.tabs.radios_shown &&
            console.tabs.logo_shown && console.tabs.side_shown &&
            console.tabs.arm_value == widget_value::disabled &&
            console.tabs.core_value == widget_value::disabled &&
            std::strcmp(console.tabs.side_name, "Arm") == 0 && !console.tab_dirty[kAllegianceTab] &&
            g_tab_send.calls == 0,
        "lock 1 shows the side with the choices disabled and sends nothing"
    );

    profile_console_init(&console, &service, &session, SettingsStore{}, nullptr, nullptr, nullptr);
    receive_arm(&console, true, 0);
    profile_select_tab(&console, kAllegianceTab);
    expect_open_arm(console, "selecting the tab after the reply shows an open Arm side");
    profile_record_release(&console.profile);
}

} // namespace

int main(int argc, char** argv) {
    test_replay_dossier();
    test_allegiance_tab();
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the side prompt tests");
    const std::filesystem::path coreprime = oa::boneyards_test::coreprime_archive(argc, argv);
    if (coreprime.empty()) {
        std::fprintf(stderr, "the mod folder is required\n");
        return 1;
    }
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0)
            g_frames = std::filesystem::path(argv[i + 1]);
    }
    if (!read_catalogue())
        return 1;
    boneyards_status_widget_set_analogue_atlas_path(OA_BONEYARDS_ATLAS);
    Data data;
    data.store.mount(root / "by.ccx");
    data.store.mount(root / "totala1.hpi");
    data.store.mount(coreprime);
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int i = 0; i < OA_PALETTE_COLORS; ++i)
        data.palette.entries[i] = oa::PaletteEntry{raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};
    rover_session_set_virtual_files(read_asset, &data);

    test_prompt_for_previous_core(&data);
    test_prompt_for_previous_arm(&data);
    test_no_prompt(
        &data, 'A', 'C', 42, false, "a member who already has a side opens the Main Frame"
    );
    test_no_prompt(
        &data, 'U', 'C', 42, true, "a guest login opens the Main Frame without the prompt"
    );
    test_no_prompt(
        &data,
        'U',
        'C',
        0x80000000U,
        false,
        "a guest user id opens the Main Frame without the prompt"
    );
    test_choice_waits_for_a_radio(&data);
    test_exit_leaves(&data);
    for (const char* tag : kLanguageTags)
        test_layout_in(&data, tag);
    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());

    boneyards_status_widget_set_analogue_atlas_path(nullptr);
    rover_session_set_virtual_files(nullptr, nullptr);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("side prompt tests passed\n");
    return 0;
}
