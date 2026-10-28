// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The first-run gate, and the coach marks at Compact, Regular and Large; the
// chosen language card alone marked as the carousel turns; the header's
// mark and name left out in a window; the Locate step's title and button;
// the setup's scale on a window; the game and copying steps' lists,
// which scroll when they do not fit and follow the row they show; and the
// question a changed install asks over the copying step.

#include "oa/ui/first_run/first_run.hpp"

#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/kit/looks.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

namespace {

using oa::ui::first_run::Ask;
using oa::ui::first_run::Card;
using oa::ui::first_run::CardKind;
using oa::ui::first_run::CatalogueNote;
using oa::ui::first_run::GateFacts;
using oa::ui::first_run::Hold;
using oa::ui::first_run::Leave;
using oa::ui::first_run::Model;
using oa::ui::first_run::PackState;
using oa::ui::first_run::Session;
using oa::ui::first_run::Step;
using oa::ui::first_run::Walk;

bool meets(const oa::ui::kit::Rect& left, const oa::ui::kit::Rect& right) {
    if (left.width <= 0 || left.height <= 0 || right.width <= 0 || right.height <= 0)
        return false;
    const oa::ui::kit::Rect shared = oa::ui::kit::intersect(left, right);
    return shared.width > 0 && shared.height > 0;
}

void expect_hold(GateFacts facts, Hold hold, const char* name) {
    const Hold decided = oa::ui::first_run::decide(facts);
    if (decided != hold) {
        std::fprintf(stderr, "gate %s decided %d\n", name, static_cast<int>(decided));
        OA_CHECK(decided == hold);
    }
}

void check_gate() {
    expect_hold({}, Hold::show, "a played start");
    GateFacts done;
    done.done = true;
    expect_hold(done, Hold::done, "done");
    GateFacts headless;
    headless.headless_check = true;
    headless.unattended = true;
    expect_hold(headless, Hold::headless_check, "headless check");
    GateFacts preferences;
    preferences.preferences_file = true;
    preferences.unattended = true;
    expect_hold(preferences, Hold::preferences_file, "preferences file");
    GateFacts benchmark;
    benchmark.benchmark = true;
    benchmark.unattended = true;
    expect_hold(benchmark, Hold::benchmark, "benchmark");
    GateFacts stage;
    stage.stage = true;
    stage.unattended = true;
    expect_hold(stage, Hold::stage, "stage");
    GateFacts fark;
    fark.fark = true;
    fark.unattended = true;
    expect_hold(fark, Hold::fark, "fark");
    GateFacts unattended;
    unattended.unattended = true;
    expect_hold(unattended, Hold::unattended, "unattended");
    GateFacts forced;
    forced.force = true;
    forced.done = true;
    forced.headless_check = true;
    forced.preferences_file = true;
    forced.benchmark = true;
    forced.stage = true;
    forced.fark = true;
    forced.unattended = true;
    expect_hold(forced, Hold::show, "--first-run");
    OA_CHECK(oa::ui::first_run::hold_reason(Hold::headless_check) == "headless check");
    OA_CHECK(oa::ui::first_run::hold_reason(Hold::preferences_file) == "preferences file");
    OA_CHECK(oa::ui::first_run::hold_reason(Hold::stage) == "stage");
    OA_CHECK(oa::ui::first_run::hold_reason(Hold::fark) == "fark");
    OA_CHECK(oa::ui::first_run::hold_reason(Hold::show).empty());
    OA_CHECK(oa::ui::first_run::step_named("language") == Step::language);
    OA_CHECK(oa::ui::first_run::step_named("game") == Step::game);
    OA_CHECK(oa::ui::first_run::step_named("copying") == Step::copying);
    OA_CHECK(oa::ui::first_run::step_named("online") == Step::online);
    OA_CHECK(oa::ui::first_run::step_named("buttons") == Step::buttons);
    OA_CHECK(!oa::ui::first_run::step_named("language-notice"));
    OA_CHECK(std::string(oa::ui::first_run::preference_key) == "open-annihilation.first-run");
    OA_CHECK(std::string(oa::ui::first_run::preference_done) == "done");
}

Session buttons_session(oa::ui::kit::Rect library, oa::ui::kit::Rect settings) {
    Session session;
    session.model.step = Step::buttons;
    session.model.library_button = library;
    session.model.settings_button = settings;
    session.model.library_count = 2;
    return session;
}

void check_coaches(
    oa::ui::kit::SizeClass size_class, int32_t width, int32_t height, bool short_right
) {
    oa::ui::kit::Frame frame;
    frame.area = {0, 0, width, height};
    frame.size_class = size_class;
    frame.scale_percent = 100;
    constexpr int32_t kSide = 48;
    oa::ui::kit::Rect settings{width - 24 - kSide, 16, kSide, kSide};
    oa::ui::kit::Rect library{settings.x - 12 - kSide, 16, kSide, kSide};
    if (short_right) {
        settings = {width - 4 - kSide, 16, kSide, kSide};
        library = {settings.x - 8 - kSide, 16, kSide, kSide};
    }
    const Session session = buttons_session(library, settings);
    const auto scene = oa::ui::first_run::lay_out(session, frame, nullptr);
    OA_CHECK(scene.coach_library.width > 0);
    OA_CHECK(scene.coach_settings.width > 0);
    OA_CHECK(oa::ui::kit::wholly_in(scene.coach_library, frame.area));
    OA_CHECK(oa::ui::kit::wholly_in(scene.coach_settings, frame.area));
    OA_CHECK(!meets(scene.coach_library, library));
    OA_CHECK(!meets(scene.coach_library, settings));
    OA_CHECK(!meets(scene.coach_settings, library));
    OA_CHECK(!meets(scene.coach_settings, settings));
    OA_CHECK(!meets(scene.coach_library, scene.coach_settings));
    if (short_right) {
        const bool library_flipped =
            scene.coach_library.x + scene.coach_library.width <= library.x ||
            scene.coach_library.y >= library.y + library.height ||
            scene.coach_library.y + scene.coach_library.height <= library.y;
        OA_CHECK(library_flipped);
    }

    const int32_t card_width = oa::ui::first_run::coach_card_width(size_class);
    const auto placed =
        oa::ui::first_run::place_coaches(frame.area, {}, library, settings, card_width, 96, 96);
    OA_CHECK(!meets(placed.library, library));
    OA_CHECK(!meets(placed.library, settings));
    OA_CHECK(!meets(placed.settings, library));
    OA_CHECK(!meets(placed.settings, settings));
    OA_CHECK(!meets(placed.library, placed.settings));
}

void check_keys() {
    Session session;
    Card english;
    english.tag = "en";
    english.own_name = "English";
    english.english_name = "English";
    english.kind = CardKind::built_in;
    english.current = true;
    Card german;
    german.tag = "de";
    german.own_name = "Deutsch";
    german.english_name = "German";
    german.kind = CardKind::built_in;
    session.model.cards = {english, german};
    session.carousel.selected = 0;
    oa::ui::first_run::bind(session);
    oa::ui::kit::Frame frame;
    frame.area = {0, 0, 1280, 720};
    frame.size_class = oa::ui::kit::SizeClass::large;
    const auto scene = oa::ui::first_run::lay_out(session, frame, nullptr);
    oa::ui::first_run::ensure_focus(session, scene.list);
    const auto escape = oa::ui::first_run::key(session, scene, oa::ui::kit::Key::escape);
    OA_CHECK(escape.ask == Ask::none);
    const auto right = oa::ui::first_run::key(session, scene, oa::ui::kit::Key::right);
    OA_CHECK(right.redraw);
    OA_CHECK(session.carousel.selected == 1);
    session.model.step = Step::online;
    session.focus_ready = false;
    const auto online = oa::ui::first_run::lay_out(session, frame, nullptr);
    oa::ui::first_run::ensure_focus(session, online.list);
    const auto back = oa::ui::first_run::key(session, online, oa::ui::kit::Key::escape);
    OA_CHECK(back.ask == Ask::back);
    OA_CHECK(session.model.online_on);
    const auto off = oa::ui::first_run::key(session, online, oa::ui::kit::Key::down);
    OA_CHECK(off.ask == Ask::choose_off);
    OA_CHECK(!session.model.online_on);

    Card pack;
    pack.tag = "en-XA";
    pack.own_name = "Pseudo";
    pack.kind = CardKind::pack;
    pack.pack = PackState::downloading;
    session.model.step = Step::language;
    session.model.cards = {english, pack};
    session.model.catalogue = CatalogueNote::none;
    session.carousel.selected = 0;
    session.focus_ready = false;
    oa::ui::first_run::bind(session);
    OA_CHECK(oa::ui::first_run::download_busy(session.model));
    const auto busy = oa::ui::first_run::lay_out(session, frame, nullptr);
    const oa::ui::kit::Control* next =
        oa::ui::kit::control_of(busy.list, oa::ui::first_run::next_id);
    OA_CHECK(next != nullptr && !next->enabled);
    bool saw_card = false;
    for (const oa::ui::kit::Control& control : busy.list.controls) {
        if (control.name == "first-run.language.card.en")
            saw_card = true;
        if (control.name == "first-run.language.skip")
            OA_CHECK(control.enabled);
    }
    OA_CHECK(saw_card);
}

/// The step line the setup chrome will draw.
///
/// @param scene the layout
/// @return the filled line; empty when the step has no chrome
std::string chrome_step(const oa::ui::first_run::Scene& scene) {
    for (const oa::ui::kit::Item& item : scene.list.items) {
        const auto* look = std::get_if<oa::ui::kit::SetupChromeLook>(&item.look);
        if (look == nullptr)
            continue;
        oa::ui::kit::StepLook step;
        step.name = look->step_name;
        step.pattern = look->step_pattern;
        step.index = look->step_index;
        step.count = look->step_count;
        return oa::ui::kit::step_line(step);
    }
    return {};
}

/// The left footer's words.
///
/// @param scene the layout
/// @return the caption; empty when the step has no chrome
std::string chrome_skip(const oa::ui::first_run::Scene& scene) {
    for (const oa::ui::kit::Item& item : scene.list.items) {
        const auto* look = std::get_if<oa::ui::kit::SetupChromeLook>(&item.look);
        if (look != nullptr)
            return look->skip;
    }
    return {};
}

/// Tells whether a scene draws a line.
///
/// @param scene the layout
/// @param text the line
/// @return true when an item shows it
bool shows_line(const oa::ui::first_run::Scene& scene, std::string_view text) {
    for (const oa::ui::kit::Item& item : scene.list.items)
        if (item.text == text)
            return true;
    return false;
}

/// A fake catalogue that marks a text, so a missed lookup stays English.
///
/// @param scene a step, laid out
/// @param line the line it should show
void expect_line(const oa::ui::first_run::Scene& scene, std::string_view line, bool chrome) {
    const std::string got = chrome ? chrome_step(scene) : std::string{};
    const bool shown = chrome ? got == line : shows_line(scene, line);
    if (!shown) {
        std::fprintf(stderr, "step line: %s\n", chrome ? got.c_str() : "(not in the list)");
        OA_CHECK(shown);
    }
}

/// The step line and the sample Library are looked up. A catalogue that marks
/// each text changes them; the sample changes only when its hook is set.
void check_lookup() {
    oa::ui::kit::Frame frame;
    frame.area = {0, 0, 1280, 720};
    frame.size_class = oa::ui::kit::SizeClass::large;
    frame.scale_percent = 100;
    Session welcome;
    welcome.model.step = Step::language;
    const auto welcome_scene = oa::ui::first_run::lay_out(welcome, frame, nullptr);
    expect_line(welcome_scene, "WELCOME · 1 OF 3", true);
    Session online;
    online.model.step = Step::online;
    expect_line(oa::ui::first_run::lay_out(online, frame, nullptr), "ONLINE · 2 OF 3", true);
    const Session buttons = buttons_session({100, 16, 48, 48}, {160, 16, 48, 48});
    expect_line(
        oa::ui::first_run::lay_out(buttons, frame, nullptr), "THE MAIN MENU · 3 OF 3", false
    );
    Session setup;
    setup.model.walk = Walk::setup;
    setup.model.step = Step::language;
    expect_line(oa::ui::first_run::lay_out(setup, frame, nullptr), "LANGUAGE · 1 OF 5", true);
    setup.model.step = Step::game;
    expect_line(oa::ui::first_run::lay_out(setup, frame, nullptr), "THE GAME · 2 OF 5", true);
    setup.model.step = Step::copying;
    expect_line(oa::ui::first_run::lay_out(setup, frame, nullptr), "COPYING · 3 OF 5", true);
    setup.model.step = Step::online;
    expect_line(oa::ui::first_run::lay_out(setup, frame, nullptr), "ONLINE · 4 OF 5", true);
    setup.model.step = Step::buttons;
    setup.model.library_button = {100, 16, 48, 48};
    setup.model.settings_button = {160, 16, 48, 48};
    expect_line(oa::ui::first_run::lay_out(setup, frame, nullptr), "THE MAIN MENU · 5 OF 5", false);

    Model english;
    const std::string plain = oa::ui::first_run::sample_words(english);
    OA_CHECK(plain.find("Active\n") != std::string::npos);
    OA_CHECK(plain.find("Unload mod\n") != std::string::npos);
    OA_CHECK(plain.find("Built in\n") != std::string::npos);
    OA_CHECK(plain.find("~") == std::string::npos);

    const char* catalogue_text = "[{step} · {index} OF {count}]\n"
                                 "{\n"
                                 "fr=^{step}|{index}|{count}^;\n"
                                 "}\n"
                                 "[WELCOME]\n"
                                 "{\n"
                                 "fr=~WELCOME~;\n"
                                 "}\n"
                                 "[ONLINE]\n"
                                 "{\n"
                                 "fr=~ONLINE~;\n"
                                 "}\n"
                                 "[THE MAIN MENU]\n"
                                 "{\n"
                                 "fr=~THE MAIN MENU~;\n"
                                 "}\n"
                                 "[Active]\n"
                                 "{\n"
                                 "fr=~Active~;\n"
                                 "}\n"
                                 "[Mods]\n"
                                 "{\n"
                                 "fr=~Mods~;\n"
                                 "}\n"
                                 "[Playing]\n"
                                 "{\n"
                                 "fr=~Playing~;\n"
                                 "}\n"
                                 "[Unload mod]\n"
                                 "{\n"
                                 "fr=~Unload mod~;\n"
                                 "}\n"
                                 "[Built in]\n"
                                 "{\n"
                                 "fr=~Built in~;\n"
                                 "}\n";
    oa::data::languages::InterfaceText catalogue;
    OA_CHECK(catalogue.add(catalogue_text));
    const oa::data::languages::Language* french = oa::data::languages::find_by_tag("fr");
    OA_CHECK(french != nullptr);
    if (french == nullptr)
        return;
    oa::data::languages::set_interface_language(&catalogue, *french);

    expect_line(oa::ui::first_run::lay_out(welcome, frame, nullptr), "^~WELCOME~|1|3^", true);
    expect_line(oa::ui::first_run::lay_out(online, frame, nullptr), "^~ONLINE~|2|3^", true);
    expect_line(
        oa::ui::first_run::lay_out(buttons, frame, nullptr), "^~THE MAIN MENU~|3|3^", false
    );

    // The catalogue is installed, and the sample still shows English until it
    // uses the same lookup the live Library uses.
    OA_CHECK(oa::ui::first_run::sample_words(english).find("~") == std::string::npos);
    english.sample_text.shown = [](void*, std::string_view text) {
        return oa::data::languages::interface_text(text);
    };
    const std::string marked = oa::ui::first_run::sample_words(english);
    OA_CHECK(marked.find("~Active~\n") != std::string::npos);
    OA_CHECK(marked.find("~Mods~\n") != std::string::npos);
    OA_CHECK(marked.find("~Playing~\n") != std::string::npos);
    OA_CHECK(marked.find("~Unload mod~\n") != std::string::npos);
    OA_CHECK(marked.find("~Built in~\n") != std::string::npos);

    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
}

Card named_card(std::string_view tag, std::string_view own_name) {
    Card card;
    card.tag = std::string(tag);
    card.own_name = std::string(own_name);
    card.english_name = std::string(own_name);
    card.kind = CardKind::built_in;
    return card;
}

const oa::ui::kit::CarouselSlot* slot_of(const oa::ui::first_run::Scene& scene, int32_t index) {
    for (const oa::ui::kit::CarouselSlot& slot : scene.places)
        if (slot.index == index)
            return &slot;
    return nullptr;
}

void check_wrap_click_and_buttons() {
    Session session;
    session.model.cards = {
        named_card("en", "English"),
        named_card("de", "Deutsch"),
        named_card("es", "Espanol"),
        named_card("fr", "Francais"),
        named_card("it", "Italiano"),
    };
    session.carousel.selected = 0;
    oa::ui::first_run::bind(session);
    oa::ui::kit::Frame frame;
    frame.area = {0, 0, 1280, 720};
    frame.size_class = oa::ui::kit::SizeClass::large;
    const auto scene = oa::ui::first_run::lay_out(session, frame, nullptr);
    oa::ui::first_run::ensure_focus(session, scene.list);
    const auto left = oa::ui::first_run::key(session, scene, oa::ui::kit::Key::left);
    OA_CHECK(left.redraw);
    OA_CHECK(session.carousel.selected == 4);
    const int32_t started = oa::ui::kit::carousel_index_milli(session.carousel);
    OA_CHECK(started - 4 * 1000 == 1000);
    oa::ui::first_run::tick(session, 20);
    OA_CHECK(oa::ui::kit::carousel_index_milli(session.carousel) < started);

    Session wheeled;
    wheeled.model.cards = session.model.cards;
    wheeled.carousel.selected = 0;
    oa::ui::first_run::bind(wheeled);
    const auto wheel_scene = oa::ui::first_run::lay_out(wheeled, frame, nullptr);
    const auto turned = oa::ui::first_run::wheel(wheeled, wheel_scene, 1.F);
    OA_CHECK(turned.redraw);
    OA_CHECK(wheeled.carousel.selected == 4);
    const int32_t wheel_start = oa::ui::kit::carousel_index_milli(wheeled.carousel);
    oa::ui::first_run::tick(wheeled, 20);
    OA_CHECK(oa::ui::kit::carousel_index_milli(wheeled.carousel) < wheel_start);

    Session picking;
    picking.model.cards = session.model.cards;
    picking.carousel.selected = 0;
    oa::ui::first_run::bind(picking);
    const auto pick_scene = oa::ui::first_run::lay_out(picking, frame, nullptr);
    const oa::ui::kit::CarouselSlot* side = slot_of(pick_scene, 1);
    OA_CHECK(side != nullptr);
    if (side != nullptr) {
        const oa::ui::kit::Point at{
            side->face.x + side->face.width / 2, side->face.y + side->face.height / 2
        };
        const auto down = oa::ui::first_run::pointer(
            picking, pick_scene, oa::ui::first_run::PointerKind::down, at, 0, 2
        );
        OA_CHECK(down.ask == Ask::none);
        const auto up = oa::ui::first_run::pointer(
            picking, pick_scene, oa::ui::first_run::PointerKind::up, at, 0, 2
        );
        OA_CHECK(up.ask == Ask::pick);
        OA_CHECK(up.index == side->index);
        OA_CHECK(picking.carousel.selected == side->index);
    }

    Session centring;
    centring.model.cards = session.model.cards;
    centring.carousel.selected = 0;
    oa::ui::first_run::bind(centring);
    const auto centre_scene = oa::ui::first_run::lay_out(centring, frame, nullptr);
    const oa::ui::kit::CarouselSlot* other = slot_of(centre_scene, 1);
    OA_CHECK(other != nullptr);
    if (other != nullptr) {
        const oa::ui::kit::Point at{
            other->face.x + other->face.width / 2, other->face.y + other->face.height / 2
        };
        std::ignore = oa::ui::first_run::pointer(
            centring, centre_scene, oa::ui::first_run::PointerKind::down, at, 0, 1
        );
        const auto up = oa::ui::first_run::pointer(
            centring, centre_scene, oa::ui::first_run::PointerKind::up, at, 0, 1
        );
        OA_CHECK(up.ask == Ask::none);
        OA_CHECK(up.redraw);
        OA_CHECK(centring.carousel.selected == other->index);
    }

    Session buttons;
    buttons.model.step = Step::buttons;
    buttons.model.cards = session.model.cards;
    const auto button_scene = oa::ui::first_run::lay_out(buttons, frame, nullptr);
    bool saw_skip = false;
    bool saw_back = false;
    bool saw_start = false;
    bool saw_pulse = false;
    for (const oa::ui::kit::Control& control : button_scene.list.controls) {
        if (control.name == "first-run.buttons.skip")
            saw_skip = true;
        if (control.name == "first-run.buttons.back")
            saw_back = true;
        if (control.name == "first-run.buttons.start")
            saw_start = true;
    }
    for (const oa::ui::kit::Item& item : button_scene.list.items) {
        if (item.role == oa::ui::kit::Role::call_to_action &&
            std::holds_alternative<oa::ui::kit::CallToActionLook>(item.look))
            saw_pulse = true;
    }
    OA_CHECK(!saw_skip);
    OA_CHECK(saw_back);
    OA_CHECK(saw_start);
    OA_CHECK(saw_pulse);
    buttons.interaction.focused = oa::ui::first_run::back_id;
    const auto enter = oa::ui::first_run::key(buttons, button_scene, oa::ui::kit::Key::enter);
    OA_CHECK(enter.ask == Ask::advance);
    const auto escape = oa::ui::first_run::key(buttons, button_scene, oa::ui::kit::Key::escape);
    OA_CHECK(escape.ask == Ask::advance);
}

/// The caption of Next.
///
/// @param scene the layout
/// @return the caption; empty when Next is not there
std::string next_caption(const oa::ui::first_run::Scene& scene) {
    const oa::ui::kit::Control* next =
        oa::ui::kit::control_of(scene.list, oa::ui::first_run::next_id);
    if (next == nullptr)
        return {};
    return next->text;
}

/// The note and Next follow a readable language in the middle, then the
/// language in use. A pack that is not installed leaves both as they are.
void check_centred_language() {
    const char* catalogue_text =
        "[Next]\n"
        "{\n"
        "de=~de-next~;\n"
        "fr=~fr-next~;\n"
        "}\n"
        "[Other languages download from Core Prime. The first download makes a random install "
        "ID, which a later step explains.]\n"
        "{\n"
        "de=~de-note~;\n"
        "fr=~fr-note~;\n"
        "}\n";
    oa::data::languages::InterfaceText catalogue;
    OA_CHECK(catalogue.add(catalogue_text));
    const oa::data::languages::Language* french = oa::data::languages::find_by_tag("fr");
    const oa::data::languages::Language* german = oa::data::languages::find_by_tag("de");
    OA_CHECK(french != nullptr && german != nullptr);
    if (french == nullptr || german == nullptr)
        return;
    oa::data::languages::set_interface_language(&catalogue, *french);
    OA_CHECK(&oa::data::languages::interface_language() == french);

    Card french_card = named_card("fr", "Francais");
    french_card.current = true;
    Card german_card = named_card("de", "Deutsch");
    Card pack;
    pack.tag = "pt";
    pack.own_name = "Portugues";
    pack.english_name = "Portuguese";
    pack.kind = CardKind::pack;
    pack.pack = PackState::ready;

    oa::ui::kit::Frame frame;
    frame.area = {0, 0, 1280, 720};
    frame.size_class = oa::ui::kit::SizeClass::large;

    Session session;
    session.model.step = Step::language;
    session.model.cards = {french_card, german_card, pack};
    session.carousel.selected = 1;
    oa::ui::first_run::bind(session);
    const auto centred = oa::ui::first_run::lay_out(session, frame, nullptr);
    OA_CHECK(next_caption(centred) == "~de-next~");
    OA_CHECK(shows_line(centred, "~de-note~"));
    OA_CHECK(&oa::data::languages::interface_language() == french);

    session.carousel.selected = 0;
    oa::ui::first_run::bind(session);
    const auto home = oa::ui::first_run::lay_out(session, frame, nullptr);
    OA_CHECK(next_caption(home) == "~fr-next~");
    OA_CHECK(shows_line(home, "~fr-note~"));
    OA_CHECK(!shows_line(home, "~de-note~"));

    session.carousel.selected = 2;
    oa::ui::first_run::bind(session);
    const auto waiting = oa::ui::first_run::lay_out(session, frame, nullptr);
    OA_CHECK(next_caption(waiting) == "~fr-next~");
    OA_CHECK(shows_line(waiting, "~fr-note~"));
    OA_CHECK(!shows_line(waiting, "~de-note~"));
    OA_CHECK(&oa::data::languages::interface_language() == french);

    session.model.step = Step::online;
    session.carousel.selected = 1;
    const auto online = oa::ui::first_run::lay_out(session, frame, nullptr);
    OA_CHECK(next_caption(online) == "~fr-next~");
    OA_CHECK(!shows_line(online, "~de-note~"));

    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
}

/// Presses the left footer button.
///
/// @param session the session
/// @param scene the layout
/// @return what the press asked
oa::ui::first_run::Outcome press_skip(Session& session, const oa::ui::first_run::Scene& scene) {
    const oa::ui::kit::Control* skip =
        oa::ui::kit::control_of(scene.list, oa::ui::first_run::skip_id);
    OA_CHECK(skip != nullptr && skip->enabled);
    const oa::ui::kit::Point at{
        skip->rect.x + skip->rect.width / 2, skip->rect.y + skip->rect.height / 2
    };
    std::ignore =
        oa::ui::first_run::pointer(session, scene, oa::ui::first_run::PointerKind::down, at, 0, 1);
    return oa::ui::first_run::pointer(session, scene, oa::ui::first_run::PointerKind::up, at, 0, 1);
}

/// Quit on every step of a first start, Skip on every step of a re-run.
/// The footer buttons stay inside the window, at Settings' button size.
void check_leave_word() {
    oa::ui::kit::Frame frame;
    frame.area = {0, 0, 1280, 720};
    frame.size_class = oa::ui::kit::SizeClass::large;
    const Step welcome_steps[] = {Step::language, Step::online};
    Session again;
    again.model.leave = Leave::skip;
    again.model.walk = Walk::welcome;
    for (const Step step : welcome_steps) {
        again.model.step = step;
        const auto scene = oa::ui::first_run::lay_out(again, frame, nullptr);
        OA_CHECK(chrome_skip(scene) == "Skip");
        Session pressed = again;
        OA_CHECK(press_skip(pressed, scene).ask == Ask::skip);
    }

    const Step setup_steps[] = {Step::language, Step::game, Step::copying, Step::online};
    Session first;
    first.model.leave = Leave::quit;
    first.model.walk = Walk::setup;
    first.model.copy.running = true;
    for (const Step step : setup_steps) {
        first.model.step = step;
        const auto scene = oa::ui::first_run::lay_out(first, frame, nullptr);
        OA_CHECK(chrome_skip(scene) == "Quit");
        const oa::ui::kit::Control* skip =
            oa::ui::kit::control_of(scene.list, oa::ui::first_run::skip_id);
        OA_CHECK(skip != nullptr && skip->text == "Quit");
        OA_CHECK(skip->rect.height == oa::ui::kit::compact_metrics.button_height);
        OA_CHECK(skip->rect.width >= oa::ui::kit::compact_metrics.button_width);
        Session pressed = first;
        OA_CHECK(press_skip(pressed, scene).ask == Ask::quit);
    }

    const int32_t sizes[][2] = {{640, 480}, {1280, 720}, {1344, 838}};
    for (const auto& size : sizes) {
        oa::ui::kit::Frame room;
        room.area = {0, 0, size[0], size[1]};
        room.size_class = oa::ui::kit::size_class_of(size[0], size[1]);
        room.scale_percent = 100;
        Session session;
        session.model.leave = Leave::quit;
        session.model.walk = Walk::setup;
        session.model.step = Step::language;
        const auto scene = oa::ui::first_run::lay_out(session, room, nullptr);
        for (const oa::ui::kit::Control& control : scene.list.controls) {
            const std::string_view name = control.name;
            const bool footer =
                name.ends_with(".skip") || name.ends_with(".back") || name.ends_with(".next");
            if (!footer)
                continue;
            const int32_t right = control.rect.x + control.rect.width;
            const int32_t bottom = control.rect.y + control.rect.height;
            OA_CHECK(control.rect.x >= oa::ui::kit::compact_metrics.padding);
            OA_CHECK(control.rect.y >= 0);
            OA_CHECK(right <= size[0] - oa::ui::kit::compact_metrics.edge);
            OA_CHECK(bottom <= size[1]);
            OA_CHECK(control.rect.height == oa::ui::kit::compact_metrics.button_height);
            OA_CHECK(control.rect.width >= oa::ui::kit::compact_metrics.button_width);
        }
    }
}

/// The game step's column is Settings' panel, and its rows leave room between them.
void check_game_column() {
    using oa::ui::first_run::GameSource;
    using oa::ui::first_run::SourceChip;
    using oa::ui::first_run::SourceMark;
    const int32_t sizes[][2] = {{640, 480}, {1280, 720}, {1920, 1080}};
    for (const auto& size : sizes) {
        oa::ui::kit::Frame frame;
        frame.area = {0, 0, size[0], size[1]};
        frame.size_class = oa::ui::kit::size_class_of(size[0], size[1]);
        frame.scale_percent = 100;
        Session session;
        session.model.leave = Leave::quit;
        session.model.walk = Walk::setup;
        session.model.step = Step::game;
        GameSource fuller;
        fuller.title = "setup-full.exe";
        fuller.detail = "~/Downloads";
        fuller.mark = SourceMark::gog;
        fuller.footnote = "1.0 GB";
        fuller.chips = {
            SourceChip{"Total Annihilation 3.1c", true},
            SourceChip{"Campaign", true},
            SourceChip{"Core Contingency", true},
            SourceChip{"Battle Tactics", true},
            SourceChip{"Music", true},
        };
        GameSource thinner;
        thinner.title = "setup-thin.exe";
        thinner.detail = "~/Downloads";
        thinner.mark = SourceMark::gog;
        thinner.footnote = "1.0 GB";
        thinner.chips = {
            SourceChip{"Total Annihilation 3.1c", true},
            SourceChip{"Music", true},
        };
        session.model.sources.push_back(std::move(fuller));
        session.model.sources.push_back(std::move(thinner));
        session.model.source = 0;
        const auto scene = oa::ui::first_run::lay_out(session, frame, nullptr);
        const oa::ui::kit::Control* first =
            oa::ui::kit::control_of(scene.list, oa::ui::first_run::source_id);
        const oa::ui::kit::Control* second =
            oa::ui::kit::control_of(scene.list, oa::ui::first_run::source_id + 1);
        const int32_t edge =
            oa::ui::kit::compact_metrics.edge + oa::ui::kit::compact_metrics.padding;
        const int32_t band = std::max(size[0] - 2 * edge, int32_t{0});
        const oa::ui::kit::Metrics& metrics = oa::ui::kit::metrics_of(frame.size_class);
        const int32_t column = std::min(band, metrics.dialog_width);
        if (first == nullptr || second == nullptr) {
            OA_CHECK(first != nullptr && second != nullptr);
            continue;
        }
        OA_CHECK(first->rect.width == column);
        OA_CHECK(second->rect.width == column);
        OA_CHECK(first->rect.x == edge + std::max((band - column) / 2, int32_t{0}));
        OA_CHECK(second->rect.y == first->rect.y + first->rect.height + metrics.padding);
        OA_CHECK(first->rect.height > oa::ui::kit::choice_card_height());
        OA_CHECK(!meets(first->rect, second->rect));
        for (const oa::ui::kit::Item& item : scene.list.items) {
            if (item.role != oa::ui::kit::Role::pill)
                continue;
            OA_CHECK(item.text.find("no ") == std::string::npos);
            const bool in_first =
                item.rect.x >= first->rect.x && item.rect.y >= first->rect.y &&
                item.rect.x + item.rect.width <= first->rect.x + first->rect.width &&
                item.rect.y + item.rect.height <= first->rect.y + first->rect.height;
            const bool in_second =
                item.rect.x >= second->rect.x && item.rect.y >= second->rect.y &&
                item.rect.x + item.rect.width <= second->rect.x + second->rect.width &&
                item.rect.y + item.rect.height <= second->rect.y + second->rect.height;
            OA_CHECK(in_first || in_second);
            const auto* pill = std::get_if<oa::ui::kit::PillLook>(&item.look);
            OA_CHECK(pill != nullptr);
            if (pill == nullptr)
                continue;
            if (in_first)
                OA_CHECK(pill->state == oa::ui::kit::PillState::active);
            if (in_second)
                OA_CHECK(pill->state == oa::ui::kit::PillState::neutral);
        }
        const oa::ui::kit::Control* locate =
            oa::ui::kit::control_of(scene.list, oa::ui::first_run::locate_id);
        OA_CHECK(locate != nullptr);
        if (locate == nullptr)
            continue;
        OA_CHECK(locate->rect.height == oa::ui::kit::compact_metrics.button_height);
        OA_CHECK(locate->rect.y == second->rect.y + second->rect.height + metrics.padding);
        // As wide as the footer makes a button of the same words: Quit's and
        // Back's insets either side of the estimated caption.
        oa::ui::kit::SetupChromeLook same;
        same.width = size[0];
        same.height = size[1];
        same.skip = locate->text;
        const oa::ui::kit::SetupButtons footer = oa::ui::kit::setup_buttons(nullptr, same);
        OA_CHECK(locate->rect.width == footer.skip.width);
        OA_CHECK(locate->rect.height == footer.skip.height);
        OA_CHECK(
            locate->rect.width >= oa::ui::kit::estimated_width(locate->text) +
                                      2 * oa::ui::kit::compact_metrics.choice_text_inset
        );
        OA_CHECK(locate->rect.x == first->rect.x + (column - locate->rect.width) / 2);
        OA_CHECK(scene.title_text == "Locate Total Annihilation Game Files");
    }
}

/// The right-hand button's style on a laid-out step, or plain when the chrome is absent.
oa::ui::kit::ButtonStyle next_style(const oa::ui::first_run::Scene& scene) {
    for (const oa::ui::kit::Item& item : scene.list.items) {
        if (item.role != oa::ui::kit::Role::setup_chrome)
            continue;
        if (const auto* look = std::get_if<oa::ui::kit::SetupChromeLook>(&item.look))
            return look->next_style;
    }
    return oa::ui::kit::ButtonStyle::plain;
}

/// The y of the first item whose text is `words`, or -1.
int32_t line_y(const oa::ui::first_run::Scene& scene, std::string_view words) {
    for (const oa::ui::kit::Item& item : scene.list.items) {
        if (item.text == words)
            return item.rect.y;
    }
    return -1;
}

/// A finished copy reads 100%, lists only the bundles it holds, and counts down.
void check_copying_layout() {
    oa::ui::kit::Frame frame;
    frame.area = {0, 0, 1280, 720};
    frame.size_class = oa::ui::kit::SizeClass::large;

    Session session;
    session.model.walk = Walk::setup;
    session.model.leave = Leave::quit;
    session.model.step = Step::copying;
    session.model.copy.from = "GOG";
    session.model.copy.overall = "38 of 38 · 100%";
    session.model.copy.done = 1000;
    session.model.copy.whole = 1000;
    session.model.copy.checked = true;
    session.model.copy.verdict = "All files match";
    session.copy_continue_ms = 3000;

    oa::ui::first_run::CopyBundle base;
    base.name = "Total Annihilation";
    base.file = "rev31.gp3";
    base.amount = "All files match";
    base.done = 1000;
    base.whole = 1000;
    base.phase = oa::ui::first_run::CopyPhase::done;
    oa::ui::first_run::CopyBundle music;
    music.name = "Music";
    music.file = "music/9.mp3";
    music.amount = "17 of 18 tracks";
    music.done = 1000;
    music.whole = 1000;
    music.phase = oa::ui::first_run::CopyPhase::done;
    music.note = true;
    session.model.copy.bundles.push_back(std::move(base));
    session.model.copy.bundles.push_back(std::move(music));

    const auto finished = oa::ui::first_run::lay_out(session, frame, nullptr);
    OA_CHECK(next_caption(finished) == "Continue · 3");
    OA_CHECK(next_style(finished) == oa::ui::kit::ButtonStyle::accent);
    OA_CHECK(shows_line(finished, "38 of 38 · 100%"));
    OA_CHECK(shows_line(finished, "All files match"));
    OA_CHECK(shows_line(finished, "17 of 18 tracks"));
    OA_CHECK(!shows_line(finished, "music/9.mp3"));
    OA_CHECK(!shows_line(finished, "rev31.gp3"));
    OA_CHECK(!shows_line(finished, "Total Annihilation demo"));
    const int32_t overall_y = line_y(finished, "38 of 38 · 100%");
    const int32_t verdict_y = line_y(finished, "All files match");
    OA_CHECK(overall_y >= 0 && verdict_y > overall_y);
    bool panel = false;
    bool rule = false;
    int32_t panel_width = 0;
    for (const oa::ui::kit::Item& item : finished.list.items) {
        if (item.role == oa::ui::kit::Role::fill && item.colour == oa::ui::kit::colour::list) {
            panel = true;
            panel_width = item.rect.width;
        }
        if (item.role == oa::ui::kit::Role::bevel && item.colour == oa::ui::kit::colour::edge_light)
            OA_CHECK(item.rect.width == panel_width || panel_width == 0);
        if (item.role == oa::ui::kit::Role::rule)
            rule = true;
    }
    OA_CHECK(panel);
    OA_CHECK(rule);
    bool inner_bar = false;
    for (const oa::ui::kit::Item& item : finished.list.items) {
        if (item.role != oa::ui::kit::Role::progress || item.rect.height != 6)
            continue;
        inner_bar = item.rect.width == panel_width - 2 * oa::ui::kit::compact_metrics.padding;
    }
    OA_CHECK(inner_bar);
    bool tracks_hint = false;
    for (const oa::ui::kit::Item& item : finished.list.items) {
        if (item.text == "17 of 18 tracks")
            tracks_hint = item.colour == oa::ui::kit::colour::hint;
    }
    OA_CHECK(tracks_hint);

    session.model.copy.checked = false;
    session.model.copy.running = true;
    session.model.copy.verdict.clear();
    session.copy_continue_ms = -1;
    session.model.copy.overall = "12 of 38 · 31%";
    session.model.copy.bundles.front().phase = oa::ui::first_run::CopyPhase::copying;
    session.model.copy.bundles.front().file = "rev31.gp3";
    const auto running = oa::ui::first_run::lay_out(session, frame, nullptr);
    OA_CHECK(next_caption(running) == "Pause");
    OA_CHECK(next_style(running) == oa::ui::kit::ButtonStyle::plain);
    OA_CHECK(shows_line(running, "rev31.gp3"));
    OA_CHECK(!shows_line(running, "music/9.mp3"));

    session.model.copy.checked = true;
    session.model.copy.running = false;
    session.model.copy.hashing = false;
    session.model.copy.paused = false;
    session.model.copy.verdict = "All files match";
    session.model.copy.bundles.front().phase = oa::ui::first_run::CopyPhase::done;
    session.copy_continue_armed = true;
    session.copy_continue_stopped = false;
    session.copy_continue_ms = -1;
    oa::ui::first_run::tick(session, 16);
    OA_CHECK(session.copy_continue_ms == oa::ui::first_run::copy_continue_wait_ms);
    oa::ui::first_run::tick(session, 1000);
    OA_CHECK(session.copy_continue_ms == oa::ui::first_run::copy_continue_wait_ms - 1000);
    const auto counting = oa::ui::first_run::lay_out(session, frame, nullptr);
    OA_CHECK(next_caption(counting) == "Continue · 2");

    std::ignore = oa::ui::first_run::pointer(
        session, counting, oa::ui::first_run::PointerKind::move, {20, 20}, 0, 1
    );
    OA_CHECK(session.copy_continue_ms == oa::ui::first_run::copy_continue_wait_ms - 1000);
    Session wheeled = session;
    std::ignore = oa::ui::first_run::wheel(wheeled, counting, 1.F);
    OA_CHECK(wheeled.copy_continue_stopped);
    Session pressed = session;
    std::ignore = oa::ui::first_run::pointer(
        pressed, counting, oa::ui::first_run::PointerKind::down, {4, 4}, 0, 1
    );
    OA_CHECK(pressed.copy_continue_stopped);
    std::ignore = oa::ui::first_run::key(session, counting, oa::ui::kit::Key::tab);
    OA_CHECK(session.copy_continue_stopped);
    OA_CHECK(session.copy_continue_ms < 0);
    oa::ui::first_run::tick(session, 1000);
    OA_CHECK(session.copy_continue_ms < 0);

    Session differing = session;
    differing.copy_continue_stopped = false;
    differing.copy_continue_armed = true;
    differing.copy_continue_ms = -1;
    differing.model.copy.bundles.front().phase = oa::ui::first_run::CopyPhase::differ;
    oa::ui::first_run::tick(differing, 16);
    OA_CHECK(differing.copy_continue_ms < 0);

    Session waiting = session;
    waiting.copy_continue_stopped = false;
    waiting.copy_continue_armed = false;
    waiting.copy_continue_ms = -1;
    waiting.model.copy.bundles.front().phase = oa::ui::first_run::CopyPhase::done;
    oa::ui::first_run::tick(waiting, 16);
    OA_CHECK(waiting.copy_continue_ms < 0);

    oa::ui::kit::Frame narrow;
    narrow.area = {0, 0, 640, 480};
    narrow.size_class = oa::ui::kit::size_class_of(640, 480);
    Session crowded = session;
    crowded.copy_continue_ms = 3000;
    crowded.model.copy.bundles.front().phase = oa::ui::first_run::CopyPhase::done;
    const auto small = oa::ui::first_run::lay_out(crowded, narrow, nullptr);
    const oa::ui::kit::Control* skip =
        oa::ui::kit::control_of(small.list, oa::ui::first_run::skip_id);
    const oa::ui::kit::Control* back =
        oa::ui::kit::control_of(small.list, oa::ui::first_run::back_id);
    const oa::ui::kit::Control* next =
        oa::ui::kit::control_of(small.list, oa::ui::first_run::next_id);
    OA_CHECK(skip != nullptr && back != nullptr && next != nullptr);
    OA_CHECK(next->text == "Continue · 3");
    OA_CHECK(!meets(skip->rect, back->rect));
    OA_CHECK(!meets(back->rect, next->rect));
    OA_CHECK(!meets(skip->rect, next->rect));
}

/// A bundle's row stays the same height when its file line becomes the result.
void bundle_row_keeps_its_height() {
    oa::ui::kit::Frame frame;
    frame.area = {0, 0, 1280, 720};
    frame.size_class = oa::ui::kit::SizeClass::large;

    Session session;
    session.model.walk = Walk::setup;
    session.model.leave = Leave::quit;
    session.model.step = Step::copying;
    session.model.copy.running = true;
    session.model.copy.checked = false;
    oa::ui::first_run::CopyBundle base;
    base.name = "Total Annihilation";
    base.file = "rev31.gp3";
    base.amount = "31%";
    base.done = 300;
    base.whole = 1000;
    base.phase = oa::ui::first_run::CopyPhase::copying;
    oa::ui::first_run::CopyBundle music;
    music.name = "Music";
    music.amount = "17 of 18 tracks";
    music.done = 1000;
    music.whole = 1000;
    music.phase = oa::ui::first_run::CopyPhase::done;
    music.note = true;
    session.model.copy.bundles.push_back(std::move(base));
    session.model.copy.bundles.push_back(std::move(music));

    const auto copying = oa::ui::first_run::lay_out(session, frame, nullptr);
    session.model.copy.bundles.front().phase = oa::ui::first_run::CopyPhase::done;
    session.model.copy.bundles.front().amount = "All files match";
    session.model.copy.bundles.front().file = "rev31.gp3";
    const auto done = oa::ui::first_run::lay_out(session, frame, nullptr);

    const int32_t copying_base = line_y(copying, "Total Annihilation");
    const int32_t copying_music = line_y(copying, "Music");
    const int32_t done_base = line_y(done, "Total Annihilation");
    const int32_t done_music = line_y(done, "Music");
    OA_CHECK(copying_base >= 0 && done_base >= 0);
    OA_CHECK(copying_music - copying_base == done_music - done_base);
    OA_CHECK(
        line_y(copying, "rev31.gp3") == copying_base + oa::ui::kit::compact_metrics.regular_line
    );
    OA_CHECK(
        line_y(done, "All files match") == done_base + oa::ui::kit::compact_metrics.regular_line
    );
    OA_CHECK(shows_line(copying, "31%"));
    OA_CHECK(shows_line(done, "All files match"));
    OA_CHECK(!shows_line(done, "rev31.gp3"));
    OA_CHECK(!shows_line(done, "31%"));
}

/// The band the step's words share, matching the screen's own band.
oa::ui::kit::Rect content_band(int32_t width, int32_t height) {
    const int32_t left = oa::ui::kit::compact_metrics.edge + oa::ui::kit::compact_metrics.padding;
    const int32_t top = oa::ui::kit::compact_metrics.edge +
                        oa::ui::kit::compact_metrics.header_height + 1 +
                        oa::ui::kit::compact_metrics.padding;
    const int32_t bottom = height - oa::ui::kit::compact_metrics.edge -
                           oa::ui::kit::compact_metrics.footer_height - 1 -
                           oa::ui::kit::compact_metrics.padding;
    return {left, top, std::max(width - 2 * left, int32_t{0}), std::max(bottom - top, int32_t{0})};
}

/// Half a rule over the window face, the painter's mix at 128 of 256.
oa::ui::kit::Colour expected_floor() {
    const auto channel = [](uint8_t under, uint8_t over) {
        constexpr uint32_t kOpaque = 256;
        constexpr uint32_t kShare = kOpaque / 2;
        const uint32_t painted = uint32_t{over} * kShare + kOpaque / 2;
        return static_cast<uint8_t>((uint32_t{under} * (kOpaque - kShare) + painted) / kOpaque);
    };
    return {
        channel(oa::ui::kit::colour::panel.r, oa::ui::kit::colour::rule.r),
        channel(oa::ui::kit::colour::panel.g, oa::ui::kit::colour::rule.g),
        channel(oa::ui::kit::colour::panel.b, oa::ui::kit::colour::rule.b),
    };
}

/// The language step's floor is half a rule at every size, and the note says
/// a later step explains the install ID.
void check_language_floor_and_note() {
    const oa::ui::kit::Colour floor = expected_floor();
    OA_CHECK(floor != oa::ui::kit::colour::rule);
    OA_CHECK(floor != oa::ui::kit::colour::edge_light);
    const int32_t sizes[][2] = {{640, 480}, {1280, 720}, {1344, 838}};
    for (const auto& size : sizes) {
        oa::ui::kit::Frame frame;
        frame.area = {0, 0, size[0], size[1]};
        frame.size_class = oa::ui::kit::size_class_of(size[0], size[1]);
        Session session;
        session.model.walk = Walk::setup;
        session.model.leave = Leave::quit;
        session.model.step = Step::language;
        const auto scene = oa::ui::first_run::lay_out(session, frame, nullptr);
        int rules = 0;
        std::string words;
        for (const oa::ui::kit::Item& item : scene.list.items) {
            if (item.role == oa::ui::kit::Role::rule) {
                ++rules;
                OA_CHECK(item.colour == floor);
            }
            if (!item.text.empty()) {
                if (!words.empty())
                    words.push_back(' ');
                words += item.text;
            }
        }
        OA_CHECK(rules == 6 + 13);
        OA_CHECK(words.find("next step") == std::string::npos);
        OA_CHECK(words.find("later step") != std::string::npos);
    }
}

/// Compact leaves the picture well out. Regular and Large keep it.
void check_online_well() {
    const struct Size {
        int32_t width;
        int32_t height;
        bool well;
    } sizes[] = {
        {640, 480, false},
        {800, 500, false},
        {960, 540, true},
        {1280, 720, true},
        {1344, 838, true},
    };

    for (const Size& size : sizes) {
        oa::ui::kit::Frame frame;
        frame.area = {0, 0, size.width, size.height};
        frame.size_class = oa::ui::kit::size_class_of(size.width, size.height);
        Session session;
        session.model.step = Step::online;
        const auto scene = oa::ui::first_run::lay_out(session, frame, nullptr);
        const oa::ui::kit::Rect band = content_band(size.width, size.height);
        bool saw_well = false;
        int32_t kicker_x = -1;
        int32_t kicker_y = -1;
        int32_t kicker_w = -1;
        for (const oa::ui::kit::Item& item : scene.list.items) {
            if (item.role == oa::ui::kit::Role::picture_well)
                saw_well = true;
            if (item.text == "ONLINE FEATURES") {
                kicker_x = item.rect.x;
                kicker_y = item.rect.y;
                kicker_w = item.rect.width;
            }
        }
        OA_CHECK(saw_well == size.well);
        OA_CHECK((scene.well_inset.width > 0) == size.well);
        if (!size.well) {
            OA_CHECK(kicker_x == band.x);
            OA_CHECK(kicker_y == band.y);
            OA_CHECK(kicker_w == band.width);
            OA_CHECK(!scene.sample);
        }
    }
}

/// The look of the carousel a scene draws.
///
/// @param scene the layout
/// @return the look; null when the scene has no carousel
const oa::ui::kit::CarouselLook* carousel_look(const oa::ui::first_run::Scene& scene) {
    for (const oa::ui::kit::Item& item : scene.list.items)
        if (const auto* look = std::get_if<oa::ui::kit::CarouselLook>(&item.look))
            return look;
    return nullptr;
}

/// Checks that the chosen card, and only it, is marked: the focus is on it,
/// the carousel rings it, and no other card is drawn Active.
///
/// @param session the session after ensure_focus
/// @param scene its layout
/// @param speaks true when the step speaks the chosen card's language
/// @param when what the player has just done, for the log
void expect_marked(
    const Session& session, const oa::ui::first_run::Scene& scene, bool speaks, const char* when
) {
    const int32_t chosen = session.carousel.selected;
    const oa::ui::kit::ControlId focused = session.interaction.focused;
    const bool on_chosen = focused == oa::ui::first_run::card_id + chosen;
    const oa::ui::kit::CarouselLook* look = carousel_look(scene);
    bool ringed = look != nullptr && look->chosen == chosen;
    int32_t active = 0;
    bool active_chosen = false;
    if (look != nullptr) {
        for (std::size_t at = 0; at < look->cards.size(); ++at) {
            if (look->cards[at].availability != oa::ui::kit::LanguageAvailability::active)
                continue;
            ++active;
            active_chosen = static_cast<int32_t>(at) == chosen;
        }
    }
    const bool active_right = speaks ? active == 1 && active_chosen : active == 0;
    // The focus ring is drawn around the focused control: the chosen card's face.
    const oa::ui::kit::Control* control = oa::ui::kit::control_of(scene.list, focused);
    const oa::ui::kit::CarouselSlot* slot = slot_of(scene, chosen);
    const bool ring_on_face = control != nullptr && slot != nullptr &&
                              control->rect.x == slot->face.x && control->rect.y == slot->face.y &&
                              control->rect.width == slot->face.width;
    if (!on_chosen || !ringed || !active_right || !ring_on_face) {
        std::fprintf(
            stderr,
            "%s: chosen %d, focus %d, ring %d, %d active\n",
            when,
            chosen,
            static_cast<int>(focused) - static_cast<int>(oa::ui::first_run::card_id),
            look != nullptr ? look->chosen : -2,
            active
        );
    }
    OA_CHECK(on_chosen);
    OA_CHECK(ringed);
    OA_CHECK(active_right);
    OA_CHECK(ring_on_face);
}

/// Left, left, right on the language carousel: after every key, part way
/// through the move and once it has settled, the box is on the chosen card
/// and no longer around English. The wheel, a press on a side card and a
/// carousel that slides flat do the same.
void check_box_follows_choice() {
    for (const bool flat : {false, true}) {
        Session session;
        session.model.walk = Walk::setup;
        session.model.cards = {
            named_card("en", "English"),
            named_card("de", "Deutsch"),
            named_card("es", "Espanol"),
            named_card("fr", "Francais"),
            named_card("it", "Italiano"),
        };
        session.model.cards[0].current = true;
        session.carousel.selected = 0;
        oa::ui::first_run::bind(session);
        oa::ui::kit::carousel_settle(session.carousel);
        oa::ui::kit::Frame frame;
        frame.area = {0, 0, 1280, 720};
        frame.size_class = oa::ui::kit::SizeClass::large;
        frame.scale_percent = 100;
        frame.reduced_motion = flat;
        auto scene = oa::ui::first_run::lay_out(session, frame, nullptr);
        oa::ui::first_run::ensure_focus(session, scene.list);
        expect_marked(session, scene, true, "start");
        const oa::ui::kit::Key keys[] = {
            oa::ui::kit::Key::left, oa::ui::kit::Key::left, oa::ui::kit::Key::right
        };
        for (const oa::ui::kit::Key pressed : keys) {
            const auto moved = oa::ui::first_run::key(session, scene, pressed);
            OA_CHECK(moved.redraw);
            // The focus follows at once, before the screen is laid out again.
            OA_CHECK(
                session.interaction.focused ==
                oa::ui::first_run::card_id + session.carousel.selected
            );
            oa::ui::first_run::tick(session, oa::ui::kit::carousel_move_ms / 4);
            scene = oa::ui::first_run::lay_out(session, frame, nullptr);
            oa::ui::first_run::ensure_focus(session, scene.list);
            expect_marked(session, scene, true, "moving");
            oa::ui::first_run::tick(session, oa::ui::kit::carousel_move_ms);
            scene = oa::ui::first_run::lay_out(session, frame, nullptr);
            oa::ui::first_run::ensure_focus(session, scene.list);
            expect_marked(session, scene, true, "settled");
        }
        OA_CHECK(session.carousel.selected == 4);
        // English is still the language in use, but is not marked.
        const oa::ui::kit::CarouselLook* look = carousel_look(scene);
        OA_CHECK(look != nullptr);
        if (look != nullptr && !look->cards.empty()) {
            OA_CHECK(look->cards[0].availability == oa::ui::kit::LanguageAvailability::built_in);
            OA_CHECK(look->cards[0].pill == "Built in");
            OA_CHECK(look->cards[4].pill == "Active");
        }

        const auto turned = oa::ui::first_run::wheel(session, scene, -1.F);
        OA_CHECK(turned.redraw);
        oa::ui::first_run::tick(session, oa::ui::kit::carousel_move_ms);
        scene = oa::ui::first_run::lay_out(session, frame, nullptr);
        oa::ui::first_run::ensure_focus(session, scene.list);
        expect_marked(session, scene, true, "wheel");

        const oa::ui::kit::CarouselSlot* side = slot_of(scene, 1);
        OA_CHECK(side != nullptr);
        if (side != nullptr) {
            const oa::ui::kit::Point at{
                side->face.x + side->face.width / 2, side->face.y + side->face.height / 2
            };
            std::ignore = oa::ui::first_run::pointer(
                session, scene, oa::ui::first_run::PointerKind::down, at, 0, 1
            );
            std::ignore = oa::ui::first_run::pointer(
                session, scene, oa::ui::first_run::PointerKind::up, at, 0, 1
            );
            OA_CHECK(session.carousel.selected == 1);
            oa::ui::first_run::tick(session, oa::ui::kit::carousel_move_ms / 2);
            scene = oa::ui::first_run::lay_out(session, frame, nullptr);
            oa::ui::first_run::ensure_focus(session, scene.list);
            expect_marked(session, scene, true, "press");
        }
    }

    // A pack that is not installed is ringed and focused when chosen, and
    // no card is drawn Active: the step still speaks the language in use.
    Session packed;
    packed.model.cards = {named_card("en", "English"), named_card("de", "Deutsch")};
    packed.model.cards[0].current = true;
    Card pack;
    pack.tag = "eo";
    pack.own_name = "Esperanto";
    pack.kind = CardKind::pack;
    pack.size_text = "1.2 MB";
    packed.model.cards.push_back(pack);
    packed.carousel.selected = 0;
    oa::ui::first_run::bind(packed);
    oa::ui::kit::Frame frame;
    frame.area = {0, 0, 1280, 720};
    frame.size_class = oa::ui::kit::SizeClass::large;
    frame.scale_percent = 100;
    auto scene = oa::ui::first_run::lay_out(packed, frame, nullptr);
    oa::ui::first_run::ensure_focus(packed, scene.list);
    std::ignore = oa::ui::first_run::key(packed, scene, oa::ui::kit::Key::left);
    oa::ui::first_run::tick(packed, oa::ui::kit::carousel_move_ms);
    scene = oa::ui::first_run::lay_out(packed, frame, nullptr);
    oa::ui::first_run::ensure_focus(packed, scene.list);
    OA_CHECK(packed.carousel.selected == 2);
    expect_marked(packed, scene, false, "pack");

    // A focus on an arrow stays on it while the carousel turns.
    packed.interaction.focused = oa::ui::first_run::previous_id;
    std::ignore = oa::ui::first_run::key(packed, scene, oa::ui::kit::Key::left);
    OA_CHECK(packed.interaction.focused == oa::ui::first_run::previous_id);
}

/// In a window the header leaves out the mark and the name, on every step
/// with the setup's header; full screen keeps them.
void check_brand() {
    oa::ui::kit::Frame frame;
    frame.area = {0, 0, 1280, 720};
    frame.size_class = oa::ui::kit::SizeClass::large;
    frame.scale_percent = 100;
    for (const Step step : {Step::language, Step::game, Step::copying, Step::online}) {
        for (const bool windowed : {false, true}) {
            Session session;
            session.model.walk = Walk::setup;
            session.model.step = step;
            session.model.windowed = windowed;
            const auto scene = oa::ui::first_run::lay_out(session, frame, nullptr);
            bool saw = false;
            for (const oa::ui::kit::Item& item : scene.list.items) {
                const auto* look = std::get_if<oa::ui::kit::SetupChromeLook>(&item.look);
                if (look == nullptr)
                    continue;
                saw = true;
                OA_CHECK(look->brand == !windowed);
                OA_CHECK(!look->page.empty());
                OA_CHECK(look->step_count == 5);
            }
            OA_CHECK(saw);
        }
    }
}

/// The setup's scale on the windows its sizes are stated for, and the
/// least it keeps.
void check_setup_scale() {
    using oa::ui::first_run::setup_scale_percent;
    // 640 by 480 and 960 by 540 lay out in their own points.
    OA_CHECK(setup_scale_percent(640, 480, 100, true) == 100);
    OA_CHECK(setup_scale_percent(960, 540, 100, true) == 100);
    // 1280 by 720 holds 960 by 540 at four thirds.
    OA_CHECK(setup_scale_percent(1280, 720, 100, true) == 133);
    OA_CHECK(setup_scale_percent(1920, 1080, 100, true) == 200);
    // A 2x window of 1920 by 1117 points: 3840 by 2234 pixels.
    OA_CHECK(setup_scale_percent(3840, 2234, 200, true) == 400);
    // Whole steps without fractions; the least is kept.
    OA_CHECK(setup_scale_percent(1280, 720, 100, false) == 100);
    OA_CHECK(setup_scale_percent(2560, 1440, 100, false) == 200);
    OA_CHECK(setup_scale_percent(640, 480, 200, true) == 200);
    OA_CHECK(setup_scale_percent(0, 0, 0, true) == 100);

    const struct Window {
        int32_t width;
        int32_t height;
        int32_t density;
    } windows[] = {{640, 480, 1}, {1280, 720, 1}, {1920, 1080, 1}, {1920, 1117, 2}};

    for (const Window& window : windows) {
        const oa::ui::kit::Frame frame = oa::ui::first_run::setup_frame(
            window.width * window.density,
            window.height * window.density,
            window.density * 100,
            true
        );
        // The room never shrinks below 640 by 480, and a larger window keeps
        // Regular's room.
        OA_CHECK(frame.area.width >= 640 && frame.area.height >= 480);
        if (window.width >= 960)
            OA_CHECK(frame.size_class == oa::ui::kit::SizeClass::regular);
        const oa::ui::kit::Rect drawn = oa::ui::kit::to_canvas(frame.area, frame.scale_percent);
        OA_CHECK(drawn.width <= window.width * window.density);
        OA_CHECK(drawn.height <= window.height * window.density);
    }
}

/// The text item of some words.
///
/// @param scene the scene
/// @param words the words
/// @return the item; null when none shows them
const oa::ui::kit::Item* text_item(const oa::ui::first_run::Scene& scene, std::string_view words) {
    for (const oa::ui::kit::Item& item : scene.list.items) {
        if (item.role == oa::ui::kit::Role::text && item.text == words)
            return &item;
    }
    return nullptr;
}

/// Tells whether an item shows whole: it has no clip, or lies inside it.
///
/// @param item the item
/// @return true when none of it is clipped away
bool shows_whole(const oa::ui::kit::Item& item) {
    if (item.clip.width <= 0 || item.clip.height <= 0)
        return true;
    return oa::ui::kit::wholly_in(item.rect, item.clip);
}

/// The copying step of seven bundles, one of them copying.
///
/// @param copying the bundle being copied
/// @return the session
Session seven_bundle_copy(int32_t copying) {
    Session session;
    session.model.walk = Walk::setup;
    session.model.leave = Leave::quit;
    session.model.step = Step::copying;
    session.model.copy.from = "setup_total_annihilation.exe";
    session.model.copy.overall = "12 of 47 · 25%";
    session.model.copy.done = 250;
    session.model.copy.whole = 1000;
    session.model.copy.running = true;
    const char* names[] = {
        "Total Annihilation 3.1c",
        "Campaign",
        "Core Contingency",
        "Battle Tactics",
        "Music",
        "Cavedog units",
        "Cavedog tilesets",
    };
    for (int32_t index = 0; index < 7; ++index) {
        oa::ui::first_run::CopyBundle row;
        row.name = names[index];
        row.whole = 1000;
        if (index < copying) {
            row.phase = oa::ui::first_run::CopyPhase::done;
            row.done = 1000;
            row.amount = "All files match";
        } else if (index == copying) {
            row.phase = oa::ui::first_run::CopyPhase::copying;
            row.file = "totala2.hpi";
            row.amount = "31%";
            row.done = 310;
        }
        session.model.copy.bundles.push_back(std::move(row));
    }
    return session;
}

/// The copying step's list scrolls inside its panel, above the two lines
/// under it, follows the bundle being copied, and stays where the player
/// scrolls it until the step opens again.
void check_copy_list_scrolls() {
    namespace kit = oa::ui::kit;
    namespace first_run = oa::ui::first_run;
    const kit::Frame frame = first_run::setup_frame(640, 480, 100, true);
    Session session = seven_bundle_copy(0);
    first_run::Scene scene = first_run::lay_out(session, frame, nullptr);
    const kit::ScrollArea first = scene.list_scroll;
    OA_CHECK(first.limit > 0);
    OA_CHECK(first.offset == 0);
    const kit::Item* note =
        text_item(scene, "If OA stops, the copy carries on from here next time.");
    const kit::Item* destination =
        text_item(scene, "To Documents › Open Annihilation › Games and Expansions");
    OA_CHECK(note != nullptr && destination != nullptr);
    if (note == nullptr || destination == nullptr)
        return;
    // The panel ends above the note, and where the copies go is under it.
    OA_CHECK(first.view.y + first.view.height <= note->rect.y);
    OA_CHECK(destination->rect.y >= note->rect.y + note->rect.height);
    OA_CHECK(
        destination->rect.y + destination->rect.height <=
        frame.area.height - kit::compact_metrics.edge - kit::compact_metrics.footer_height
    );
    for (const kit::Item& item : scene.list.items) {
        if (item.role == kit::Role::text && item.text == "Cavedog tilesets")
            OA_CHECK(oa::ui::kit::wholly_in(item.clip, first.view));
    }
    const kit::Item* started = text_item(scene, "Total Annihilation 3.1c");
    OA_CHECK(started != nullptr && shows_whole(*started));
    const kit::Item* last = text_item(scene, "Cavedog tilesets");
    OA_CHECK(last != nullptr && !shows_whole(*last));

    // It follows the bundle being copied.
    session = seven_bundle_copy(5);
    scene = first_run::lay_out(session, frame, nullptr);
    OA_CHECK(scene.list_scroll.offset > 0);
    const kit::Item* units = text_item(scene, "Cavedog units");
    OA_CHECK(units != nullptr && shows_whole(*units));
    const kit::Item* file = text_item(scene, "totala2.hpi");
    OA_CHECK(file != nullptr && shows_whole(*file));

    // The wheel scrolls it, and it stays there as the copy moves on.
    const int32_t followed = scene.list_scroll.offset;
    std::ignore = first_run::wheel(session, scene, 1.F);
    OA_CHECK(session.copy_list.held);
    OA_CHECK(session.copy_list.offset == followed - first_run::list_step);
    session.model.copy.bundles[5].phase = oa::ui::first_run::CopyPhase::done;
    session.model.copy.bundles[6].phase = oa::ui::first_run::CopyPhase::copying;
    scene = first_run::lay_out(session, frame, nullptr);
    OA_CHECK(scene.list_scroll.offset == followed - first_run::list_step);

    // Home, End, Page Down, Up and Down.
    std::ignore = first_run::key(session, scene, kit::Key::end);
    scene = first_run::lay_out(session, frame, nullptr);
    OA_CHECK(scene.list_scroll.offset == scene.list_scroll.limit);
    std::ignore = first_run::key(session, scene, kit::Key::home);
    scene = first_run::lay_out(session, frame, nullptr);
    OA_CHECK(scene.list_scroll.offset == 0);
    std::ignore = first_run::key(session, scene, kit::Key::page_down);
    scene = first_run::lay_out(session, frame, nullptr);
    OA_CHECK(scene.list_scroll.offset == scene.list_scroll.page_step);
    std::ignore = first_run::key(session, scene, kit::Key::up);
    scene = first_run::lay_out(session, frame, nullptr);
    OA_CHECK(scene.list_scroll.offset == scene.list_scroll.page_step - first_run::list_step);
    std::ignore = first_run::key(session, scene, kit::Key::down);
    scene = first_run::lay_out(session, frame, nullptr);
    OA_CHECK(scene.list_scroll.offset == scene.list_scroll.page_step);

    // Dragging the list moves it with the pointer.
    const kit::Point inside{
        scene.list_scroll.view.x + 40, scene.list_scroll.view.y + scene.list_scroll.view.height / 2
    };
    const int32_t before_drag = scene.list_scroll.offset;
    std::ignore = first_run::pointer(session, scene, first_run::PointerKind::down, inside, 0, 1);
    OA_CHECK(session.copy_list.dragging && !session.copy_list.by_bar);
    std::ignore = first_run::pointer(
        session, scene, first_run::PointerKind::move, {inside.x, inside.y - 20}, 0, 1
    );
    scene = first_run::lay_out(session, frame, nullptr);
    OA_CHECK(scene.list_scroll.offset == before_drag + 20);
    std::ignore = first_run::pointer(
        session, scene, first_run::PointerKind::up, {inside.x, inside.y - 20}, 0, 1
    );
    OA_CHECK(!session.copy_list.dragging);

    // Dragging the thumb moves the list the other way, further.
    const kit::Rect thumb = scene.list_scroll.thumb(kit::compact_metrics.least_thumb_height);
    const kit::Point grip{thumb.x + thumb.width / 2, thumb.y + thumb.height / 2};
    const int32_t before_thumb = scene.list_scroll.offset;
    std::ignore = first_run::pointer(session, scene, first_run::PointerKind::down, grip, 0, 1);
    OA_CHECK(session.copy_list.dragging && session.copy_list.by_bar);
    std::ignore = first_run::pointer(
        session, scene, first_run::PointerKind::move, {grip.x, grip.y + 10}, 0, 1
    );
    scene = first_run::lay_out(session, frame, nullptr);
    OA_CHECK(scene.list_scroll.offset > before_thumb + 10);
    std::ignore =
        first_run::pointer(session, scene, first_run::PointerKind::up, {grip.x, grip.y + 10}, 0, 1);
    OA_CHECK(!session.copy_list.dragging);

    // The step opening again follows the copy again.
    first_run::reset_copy_continue(session);
    OA_CHECK(!session.copy_list.held && session.copy_list.offset == 0);
    scene = first_run::lay_out(session, frame, nullptr);
    const kit::Item* tilesets = text_item(scene, "Cavedog tilesets");
    OA_CHECK(tilesets != nullptr && shows_whole(*tilesets));

    // After the check the list shows the first bundle that differs.
    session.model.copy.running = false;
    session.model.copy.checked = true;
    session.model.copy.verdict = "1 file differs";
    for (oa::ui::first_run::CopyBundle& row : session.model.copy.bundles) {
        row.phase = oa::ui::first_run::CopyPhase::done;
        row.amount = "All files match";
    }
    session.model.copy.bundles[2].phase = oa::ui::first_run::CopyPhase::differ;
    session.model.copy.bundles[2].amount = "1 file differs";
    scene = first_run::lay_out(session, frame, nullptr);
    const kit::Item* differs = text_item(scene, "Core Contingency");
    OA_CHECK(differs != nullptr && shows_whole(*differs));

    // A list that fits does not scroll, and the wheel leaves it.
    Session two = seven_bundle_copy(0);
    two.model.copy.bundles.resize(2);
    const first_run::Scene fits = first_run::lay_out(two, frame, nullptr);
    OA_CHECK(fits.list_scroll.limit == 0);
    OA_CHECK(fits.list_scroll.view.width == 0);
    std::ignore = first_run::wheel(two, fits, 1.F);
    OA_CHECK(!two.copy_list.held);
    for (const kit::Item& item : fits.list.items)
        OA_CHECK(item.clip.width == 0);
}

/// The game step's places scroll between the title and Locate another…, and
/// follow the place chosen or focused.
void check_source_list_scrolls() {
    namespace kit = oa::ui::kit;
    namespace first_run = oa::ui::first_run;
    const kit::Frame frame = first_run::setup_frame(640, 480, 100, true);
    Session session;
    session.model.walk = Walk::setup;
    session.model.leave = Leave::quit;
    session.model.step = Step::game;
    for (int32_t index = 0; index < 5; ++index) {
        oa::ui::first_run::GameSource source;
        source.title = "setup_total_annihilation_" + std::to_string(index) + ".exe";
        source.detail = "~/Downloads";
        source.mark = oa::ui::first_run::SourceMark::gog;
        source.footnote = "1.0 GB";
        source.chips = {{"Total Annihilation 3.1c", true}, {"Campaign", true}, {"Music", true}};
        session.model.sources.push_back(std::move(source));
    }
    session.model.source = 4;
    first_run::Scene scene = first_run::lay_out(session, frame, nullptr);
    first_run::ensure_focus(session, scene.list);
    scene = first_run::lay_out(session, frame, nullptr);
    const kit::ScrollArea& area = scene.list_scroll;
    OA_CHECK(area.limit > 0);
    OA_CHECK(area.offset == area.limit);
    const kit::Control* chosen = kit::control_of(scene.list, first_run::source_id + 4);
    const kit::Control* top = kit::control_of(scene.list, first_run::source_id);
    const kit::Control* locate = kit::control_of(scene.list, first_run::locate_id);
    OA_CHECK(chosen != nullptr && top != nullptr && locate != nullptr);
    if (chosen == nullptr || top == nullptr || locate == nullptr)
        return;
    OA_CHECK(kit::wholly_in(chosen->rect, chosen->clip));
    OA_CHECK(!kit::wholly_in(top->rect, top->clip));
    OA_CHECK(locate->rect.y >= area.view.y + area.view.height);
    // A press where the first place lies out of view does not reach it.
    const kit::Point hidden{top->rect.x + 40, top->rect.y + 2};
    OA_CHECK(!kit::contains(top->clip, hidden));
    OA_CHECK(kit::hit(scene.list, hidden) != first_run::source_id);

    // Up four times chooses the first place, and the list follows it.
    for (int32_t step = 0; step < 4; ++step) {
        std::ignore = first_run::key(session, scene, kit::Key::up);
        scene = first_run::lay_out(session, frame, nullptr);
    }
    OA_CHECK(session.model.source == 0);
    OA_CHECK(scene.list_scroll.offset == 0);
    const kit::Control* first_place = kit::control_of(scene.list, first_run::source_id);
    OA_CHECK(first_place != nullptr && kit::wholly_in(first_place->rect, first_place->clip));

    // End scrolls to the last place and holds; choosing a place follows again.
    std::ignore = first_run::key(session, scene, kit::Key::end);
    scene = first_run::lay_out(session, frame, nullptr);
    OA_CHECK(session.source_list.held);
    OA_CHECK(scene.list_scroll.offset == scene.list_scroll.limit);
    std::ignore = first_run::key(session, scene, kit::Key::down);
    scene = first_run::lay_out(session, frame, nullptr);
    OA_CHECK(session.model.source == 1);
    OA_CHECK(!session.source_list.held);
    const kit::Control* second = kit::control_of(scene.list, first_run::source_id + 1);
    OA_CHECK(second != nullptr && kit::wholly_in(second->rect, second->clip));
    OA_CHECK(session.interaction.focused == first_run::source_id + 1);

    // Following moves the list no more than it must: a place already in
    // view leaves it where it is.
    session.model.source = 4;
    std::ignore = first_run::key(session, scene, kit::Key::end);
    scene = first_run::lay_out(session, frame, nullptr);
    const int32_t at_end = scene.list_scroll.offset;
    OA_CHECK(at_end == scene.list_scroll.limit);
    std::ignore = first_run::key(session, scene, kit::Key::up);
    scene = first_run::lay_out(session, frame, nullptr);
    OA_CHECK(session.model.source == 3);
    OA_CHECK(scene.list_scroll.offset == at_end);
}
} // namespace

/// The question a changed install asks over the copying step: it names
/// each archive with both sizes, only its buttons take the keys and the
/// pointer, Choose another place has the focus and goes back to the game
/// step, as Escape does, and the walk's Quit or Skip leaves.
void check_changed_question() {
    oa::ui::kit::Frame frame;
    frame.area = {0, 0, 960, 540};
    frame.size_class = oa::ui::kit::SizeClass::regular;
    Session session;
    session.model.walk = Walk::setup;
    session.model.leave = Leave::quit;
    session.model.step = Step::copying;
    session.model.copy.from = "TotalA";
    session.model.copy.whole = 1000;
    oa::ui::first_run::CopyBundle base;
    base.name = "Total Annihilation 3.1c";
    base.whole = 1000;
    session.model.copy.bundles.push_back(base);
    session.model.copy.changed = {
        {"rev31.gp3", "Total Annihilation 3.1c", 5481300, 5481280},
        {"ccdata.ccx", "Core Contingency", 25871600, 25871597},
    };

    const oa::ui::kit::Question question =
        oa::ui::first_run::changed_question(session.model, oa::ui::kit::SizeClass::regular);
    OA_CHECK(question.title == "Your Total Annihilation has been changed");
    OA_CHECK(question.paragraphs.size() == 4);
    OA_CHECK(
        question.paragraphs.size() == 4 &&
        question.paragraphs[1].text ==
            "rev31.gp3, from Total Annihilation 3.1c, is 5481300 bytes. It should be 5481280." &&
        question.paragraphs[2].text ==
            "ccdata.ccx, from Core Contingency, is 25871600 bytes. It should be 25871597."
    );
    OA_CHECK(question.buttons.size() == 2);
    OA_CHECK(question.buttons.size() == 2 && question.buttons[0].caption == "Choose another place");
    OA_CHECK(question.buttons.size() == 2 && question.buttons[0].accent);
    OA_CHECK(question.buttons.size() == 2 && question.buttons[1].caption == "Quit");
    OA_CHECK(!oa::ui::kit::place_question(question, nullptr).cut);

    oa::ui::first_run::Scene scene = oa::ui::first_run::lay_out(session, frame, nullptr);
    OA_CHECK(scene.over_from > 0);
    OA_CHECK(scene.question.width == oa::ui::kit::regular_metrics.notice_width);
    OA_CHECK(oa::ui::kit::wholly_in(scene.question, {0, 0, frame.area.width, frame.area.height}));
    // The step's own controls are left out while the question shows.
    OA_CHECK(oa::ui::kit::control_of(scene.list, oa::ui::first_run::next_id) == nullptr);
    OA_CHECK(oa::ui::kit::control_of(scene.list, oa::ui::first_run::skip_id) == nullptr);
    const oa::ui::kit::Control* choose =
        oa::ui::kit::control_of(scene.list, oa::ui::first_run::changed_choose_id);
    const oa::ui::kit::Control* leave =
        oa::ui::kit::control_of(scene.list, oa::ui::first_run::changed_leave_id);
    const oa::ui::kit::Control* text =
        oa::ui::kit::control_of(scene.list, oa::ui::first_run::changed_text_id);
    OA_CHECK(choose != nullptr && leave != nullptr && text != nullptr);
    if (choose == nullptr || leave == nullptr || text == nullptr)
        return;
    OA_CHECK(choose->name == "first-run.copying.changed.choose");
    OA_CHECK(leave->name == "first-run.copying.changed.leave");
    OA_CHECK(text->text.find("ccdata.ccx, from Core Contingency") != std::string::npos);
    OA_CHECK(oa::ui::kit::wholly_in(choose->rect, scene.question));
    OA_CHECK(oa::ui::kit::wholly_in(leave->rect, scene.question));
    OA_CHECK(choose->rect.x < leave->rect.x);
    bool opens = false;
    for (const oa::ui::kit::Item& item : scene.question_list.items)
        opens = opens || item.text.starts_with("Some of its files are not the size");
    OA_CHECK(opens);

    oa::ui::first_run::ensure_focus(session, scene.list);
    OA_CHECK(session.interaction.focused == oa::ui::first_run::changed_choose_id);
    OA_CHECK(
        oa::ui::first_run::key(session, scene, oa::ui::kit::Key::enter).ask == Ask::cancel_copy
    );
    OA_CHECK(
        oa::ui::first_run::key(session, scene, oa::ui::kit::Key::escape).ask == Ask::cancel_copy
    );
    std::ignore = oa::ui::first_run::key(session, scene, oa::ui::kit::Key::tab);
    OA_CHECK(session.interaction.focused == oa::ui::first_run::changed_leave_id);
    OA_CHECK(oa::ui::first_run::key(session, scene, oa::ui::kit::Key::enter).ask == Ask::quit);

    const oa::ui::kit::Point on_leave{
        leave->rect.x + leave->rect.width / 2, leave->rect.y + leave->rect.height / 2
    };
    std::ignore = oa::ui::first_run::pointer(
        session, scene, oa::ui::first_run::PointerKind::down, on_leave, 0
    );
    OA_CHECK(
        oa::ui::first_run::pointer(session, scene, oa::ui::first_run::PointerKind::up, on_leave, 0)
            .ask == Ask::quit
    );
    // A press beside the question, where the step's Next was, does nothing.
    const oa::ui::kit::Point beside{frame.area.width - 20, frame.area.height - 20};
    std::ignore =
        oa::ui::first_run::pointer(session, scene, oa::ui::first_run::PointerKind::down, beside, 0);
    OA_CHECK(
        oa::ui::first_run::pointer(session, scene, oa::ui::first_run::PointerKind::up, beside, 0)
            .ask == Ask::none
    );

    session.model.leave = Leave::skip;
    scene = oa::ui::first_run::lay_out(session, frame, nullptr);
    const oa::ui::kit::Control* skip =
        oa::ui::kit::control_of(scene.list, oa::ui::first_run::changed_leave_id);
    OA_CHECK(skip != nullptr && skip->text == "Skip");
    session.interaction.focused = oa::ui::first_run::changed_leave_id;
    OA_CHECK(oa::ui::first_run::key(session, scene, oa::ui::kit::Key::enter).ask == Ask::skip);

    // Past changed_files_shown archives, one fewer is named and the rest
    // are counted.
    Model many = session.model;
    for (int32_t index = 0; index < 6; ++index)
        many.copy.changed.push_back({"tactics1.hpi", "Battle Tactics", 9, 8});
    const oa::ui::kit::Question long_list =
        oa::ui::first_run::changed_question(many, oa::ui::kit::SizeClass::compact);
    const std::size_t named = oa::ui::first_run::changed_files_shown - 1;
    OA_CHECK(long_list.paragraphs.size() == named + 3);
    OA_CHECK(
        long_list.paragraphs.size() == named + 3 &&
        long_list.paragraphs[named + 1].text == "And 3 more; the log has them all."
    );

    // Without changed archives the step is its own again.
    session.model.copy.changed.clear();
    scene = oa::ui::first_run::lay_out(session, frame, nullptr);
    OA_CHECK(scene.over_from == 0);
    OA_CHECK(oa::ui::kit::control_of(scene.list, oa::ui::first_run::changed_choose_id) == nullptr);
    OA_CHECK(oa::ui::kit::control_of(scene.list, oa::ui::first_run::next_id) != nullptr);
}

int main() {
    check_gate();
    check_coaches(oa::ui::kit::SizeClass::compact, 800, 500, false);
    check_coaches(oa::ui::kit::SizeClass::regular, 1024, 640, false);
    check_coaches(oa::ui::kit::SizeClass::large, 1280, 720, false);
    check_coaches(oa::ui::kit::SizeClass::large, 1280, 720, true);
    check_keys();
    check_wrap_click_and_buttons();
    check_lookup();
    check_leave_word();
    check_copying_layout();
    bundle_row_keeps_its_height();
    check_centred_language();
    check_game_column();
    check_language_floor_and_note();
    check_online_well();
    check_box_follows_choice();
    check_brand();
    check_setup_scale();
    check_copy_list_scrolls();
    check_source_list_scrolls();
    check_changed_question();
    return oa::test::check_exit_status();
}
