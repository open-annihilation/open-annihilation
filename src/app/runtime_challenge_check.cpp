// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-challenge: the download check over the main menu, its page, a
// passed check, a check that ran out, CANCEL, and a check that arrives
// during a skirmish. The download queue is a stand-in the window talks to
// through the probe in runtime_challenge.cpp.

#include "oa/app/runtime.hpp"
#include "oa_layer.hpp"
#include "web_link_state.hpp"

#include "oa/ui/kit/input.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

namespace {

namespace content = oa::app::content;
namespace kit = oa::ui::kit;

/// The stand-in queue.
struct FakeQueue {
    std::vector<content::DownloadView> rows{}; ///< the items the window reads
    uint64_t generation = 1;                   ///< moves whenever rows change
    std::vector<uint64_t> cancelled{};         ///< the items CANCEL named, in order
    std::vector<uint64_t> retried{};           ///< the items TRY AGAIN named, in order
};

/// Stops the check.
///
/// @param ok the condition
/// @param what why it failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error(std::string("challenge check: ") + std::string(what));
}

/// Returns the stand-in's generation.
///
/// @param context the stand-in
/// @return the generation
uint64_t fake_generation(void* context) {
    return static_cast<FakeQueue*>(context)->generation;
}

/// Returns the stand-in's items.
///
/// @param context the stand-in
/// @return the items
std::vector<content::DownloadView> fake_items(void* context) {
    return static_cast<FakeQueue*>(context)->rows;
}

/// Records a cancel.
///
/// @param context the stand-in
/// @param item the item
void fake_cancel(void* context, uint64_t item) {
    static_cast<FakeQueue*>(context)->cancelled.push_back(item);
}

/// Records a retry.
///
/// @param context the stand-in
/// @param item the item
void fake_retry(void* context, uint64_t item) {
    static_cast<FakeQueue*>(context)->retried.push_back(item);
}

/// Returns one queued item.
///
/// @param item its id
/// @param registry the registry id
/// @param registry_name the name the window shows
/// @param state where the item is
/// @return the item
content::DownloadView item_of(
    uint64_t item, std::string registry, std::string registry_name, content::DownloadState state
) {
    content::DownloadView row;
    row.item = item;
    row.target.registry = std::move(registry);
    row.target.registry_name = std::move(registry_name);
    row.state = state;
    return row;
}

/// Returns a check whose page carries the code in its query.
///
/// The address field is a decoy: the window reads the page, not this field.
///
/// @param code the code
/// @return the check
content::ChallengeView challenge_of(std::string code) {
    content::ChallengeView check;
    check.code = code;
    check.verify_url = "https://downloads.example.net/verify?c=" + std::move(code);
    check.address = "ignored.example/no";
    return check;
}

/// Returns the words a display list draws, joined.
///
/// @param list the display list
/// @return the words
std::string words_of(const kit::DisplayList& list) {
    std::string words;
    for (const kit::Item& item : list.items) {
        if (!item.text.empty())
            words += item.text;
    }
    for (const kit::Control& control : list.controls)
        words += control.text;
    return words;
}

} // namespace

void Runtime::check_challenge() {
    require(sdl_.window != nullptr, "the check needs the SDL presenter");
    FakeQueue fake;
    fake.rows.push_back(item_of(7, "ridge", "Example Registry", content::DownloadState::challenge));
    fake.rows.push_back(item_of(8, "ridge", "Example Registry", content::DownloadState::waiting));
    fake.rows.push_back(item_of(9, "other", "Other Registry", content::DownloadState::waiting));
    fake.rows[0].challenge = challenge_of("HXQ7-4KP2");
    set_challenge_probe(fake_generation, fake_items, fake_cancel, fake_retry, &fake);

    load(Screen::main_menu);
    require(engine_settings_fonts() != nullptr, "the dialog's fonts did not load");
    bool running = true;
    const auto press = [this, &running](SDL_Keycode code) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.key = code;
        event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
        event.key.down = true;
        dispatch_event(event, running);
        event.type = SDL_EVENT_KEY_UP;
        event.key.down = false;
        dispatch_event(event, running);
    };
    // A notice that is already up is closed, and the next frame lets the
    // check take its place. Enter is not sent while the check itself is up.
    const auto show_challenge = [&] {
        for (int frame = 0; frame < 12; ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
            LayerScreen* top = oa_layer().top();
            if (top != nullptr && top->name() == "challenge")
                return top;
            if (dynamic_cast<NoticeScreen*>(top) != nullptr ||
                dynamic_cast<QuestionScreen*>(top) != nullptr)
                press(SDLK_RETURN);
        }
        return oa_layer().find("challenge");
    };

    LayerScreen* screen = show_challenge();
    require(screen != nullptr, "a waiting check did not show on the main menu");
    require(oa_layer().waiting("challenge") == nullptr, "a second check is waiting");
    const kit::DisplayList listed = screen->display_list();
    const kit::Control* code =
        kit::control_of(listed, kit::control_named(listed, "challenge.code"));
    require(code != nullptr && code->text == "HXQ7-4KP2", "the code is not challenge.code's text");
    const std::string words = words_of(listed);
    require(
        words.find("downloads.example.net/verify") != std::string::npos,
        "the window does not show the page"
    );
    require(
        words.find("ignored.example") == std::string::npos, "the window shows the decoy address"
    );
    require(words.find("?c=") == std::string::npos, "the window shows the page's query");
    require(web_links_ != nullptr && web_links_->requests.empty(), "a page was opened unasked");

    press(SDLK_RETURN);
    require(
        web_links_ != nullptr && web_links_->requests.size() == 1 &&
            web_links_->requests[0] == "https://downloads.example.net/verify?c=HXQ7-4KP2",
        "OPEN THE CHECK did not record the page"
    );
    require(oa_layer().find("challenge") != nullptr, "OPEN THE CHECK closed the window");

    fake.rows[0].state = content::DownloadState::downloading;
    fake.rows[0].challenge.reset();
    ++fake.generation;
    run_frame(running);
    require(
        oa_layer().find("challenge") == nullptr, "a passed check did not close within one frame"
    );

    fake.rows[0].state = content::DownloadState::failed;
    fake.rows[0].failure = content::DownloadFailure::challenge_expired;
    fake.rows[0].challenge = challenge_of("HXQ7-4KP2");
    ++fake.generation;
    screen = show_challenge();
    require(screen != nullptr, "a check that ran out did not show");
    const kit::DisplayList expired = screen->display_list();
    const kit::Control* try_again =
        kit::control_of(expired, kit::control_named(expired, "challenge.try-again"));
    require(try_again != nullptr && try_again->text == "Try again", "Try again is not shown");
    require(
        kit::control_named(expired, "challenge.open") == kit::no_control,
        "OPEN THE CHECK is shown after the check ran out"
    );
    press(SDLK_RETURN);
    require(fake.retried.size() == 1 && fake.retried[0] == 7, "TRY AGAIN did not retry the item");
    require(oa_layer().find("challenge") != nullptr, "TRY AGAIN closed the window");

    fake.rows[0].state = content::DownloadState::challenge;
    fake.rows[0].failure = content::DownloadFailure::none;
    fake.rows[0].challenge = challenge_of("MNP3-8QR1");
    ++fake.generation;
    run_frame(running);
    screen = oa_layer().find("challenge");
    require(
        screen != nullptr && oa_layer().waiting("challenge") == nullptr,
        "a new code stacked a window"
    );
    const kit::DisplayList refreshed = screen->display_list();
    const kit::Control* next_code =
        kit::control_of(refreshed, kit::control_named(refreshed, "challenge.code"));
    require(
        next_code != nullptr && next_code->text == "MNP3-8QR1",
        "a new code did not refresh the window"
    );

    press(SDLK_ESCAPE);
    require(
        fake.cancelled.size() == 2 && fake.cancelled[0] == 7 && fake.cancelled[1] == 8,
        "CANCEL did not cancel the challenged item and that registry's waiting item, and nothing "
        "else"
    );
    require(oa_layer().find("challenge") == nullptr, "CANCEL left the window open");

    fake.rows[0].state = content::DownloadState::downloading;
    fake.rows[0].challenge.reset();
    ++fake.generation;
    run_frame(running);
    start_benchmark_skirmish();
    require(screen_ == Screen::match && match_ != nullptr, "the skirmish did not start");
    fake.rows[0].state = content::DownloadState::challenge;
    fake.rows[0].challenge = challenge_of("HXQ7-4KP2");
    ++fake.generation;
    run_frame(running);
    require(
        oa_layer().find("challenge") == nullptr && oa_layer().waiting("challenge") == nullptr,
        "a check showed during the skirmish"
    );
    leave_match();
    load(Screen::main_menu);
    screen = show_challenge();
    require(screen != nullptr, "the check did not show once the main menu returned");
    const kit::DisplayList returned = screen->display_list();
    const kit::Control* returned_code =
        kit::control_of(returned, kit::control_named(returned, "challenge.code"));
    require(
        returned_code != nullptr && returned_code->text == "HXQ7-4KP2",
        "the check on the main menu lost its code"
    );
    std::cout << "challenge check: the window, the page, the retry and the cancel\n";
}

} // namespace oa::app
