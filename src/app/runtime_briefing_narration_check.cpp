// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mission briefing's narration, heard through the SDL sound device: it
// plays while the briefing is open, SHUTUP silences it and plays it again
// and turns off once it is over, and the briefing's ways out silence it.
#include "oa/app/runtime.hpp"
#include "oa/ui/campaign/campaign.hpp"

#include <SDL3/SDL.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace oa::app {

namespace {

// SHUTUP's captions: its stage 1 while the narration is on, 0 while it is off.
constexpr std::string_view kNarrationOn = "Narration";
constexpr std::string_view kNarrationOff = "No Narration";

[[noreturn]] void fail(std::string_view what) {
    throw std::runtime_error("briefing narration check: " + std::string(what));
}

void require(bool condition, std::string_view what) {
    if (!condition)
        fail(what);
}

// <stem>-<step>.ppm beside --snapshot.
fs::path step_snapshot(const fs::path& snapshot, std::string_view step) {
    fs::path file = snapshot.parent_path() / snapshot.stem();
    file += '-' + std::string(step) + ".ppm";
    return file;
}

} // namespace

void Runtime::check_briefing_narration() {
    require(!options_.mute, "--mute plays no narration to check");
    const auto snapshot = [&](std::string_view step) {
        if (!options_.snapshot.empty())
            write_ppm(step_snapshot(options_.snapshot, step), frame_without_cursor());
    };
    const auto narrating = [&] { return audio_player_.stream_busy(); };
    // The caption SHUTUP shows.
    const auto shutup_caption = [&] {
        const auto* gadget = widget("SHUTUP");
        const auto* fields = gadget != nullptr
                                 ? std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget->fields)
                                 : nullptr;
        if (fields == nullptr)
            fail("the briefing has no SHUTUP button");
        const auto stage = widget_text_stages_.find("SHUTUP");
        return std::string(
            renderer::staged_caption(
                fields->text, stage == widget_text_stages_.end() ? 0U : stage->second
            )
        );
    };
    // The briefing as it must stand: open, SHUTUP showing `caption` and the
    // narration heard exactly while SHUTUP is on.
    const auto expect_briefing = [&](std::string_view caption, std::string_view when) {
        require(screen_ == Screen::briefing, "the briefing is not open " + std::string(when));
        require(
            shutup_caption() == caption,
            "SHUTUP shows '" + shutup_caption() + "' " + std::string(when)
        );
        require(
            narrating() == (caption == kNarrationOn),
            std::string(narrating() ? "the narration plays " : "no narration plays ") +
                std::string(when)
        );
        std::cout << "briefing narration check: " << when << ", SHUTUP '" << caption
                  << "', narration " << (narrating() ? "playing" : "silent") << '\n';
    };
    const auto open_briefing = [&](std::string_view when) {
        exercise_click("Start");
        expect_briefing(kNarrationOn, when);
    };
    // NEWGAME.GUI from the main menu.
    const auto open_new_campaign = [&] {
        exercise_click(menu::resource_name(menu::Button::single_player));
        require(screen_ == Screen::single_player, "did not reach SINGLE.GUI");
        exercise_click(entry::resource_name(entry::Button::new_campaign));
        require(screen_ == Screen::new_campaign, "New Campaign did not open NEWGAME.GUI");
    };
    // A key pressed through SDL input.
    const auto press = [&](SDL_Keycode key) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = key;
        event.key.down = true;
        bool running = true;
        dispatch_event(event, running);
    };
    const auto expect_mission = [&](std::string_view how) {
        require(
            screen_ == Screen::match && match_, std::string(how) + " did not start the mission"
        );
        require(!narrating(), "the narration plays on into the mission after " + std::string(how));
        std::cout << "briefing narration check: " << how << ", narration silent in the mission\n";
    };

    open_new_campaign();
    open_briefing("as the briefing opens");
    snapshot("open");

    // SHUTUP silences the narration at once; clicked again it plays it anew.
    exercise_click("SHUTUP");
    expect_briefing(kNarrationOff, "after SHUTUP is turned off");
    snapshot("off");
    exercise_click("SHUTUP");
    expect_briefing(kNarrationOn, "after SHUTUP is turned on again");
    snapshot("on");

    // PrevMenu silences it and goes back.
    exercise_click("PrevMenu");
    require(screen_ == Screen::new_campaign, "PrevMenu did not go back to NEWGAME.GUI");
    require(!narrating(), "the narration plays on after PrevMenu");
    std::cout << "briefing narration check: PrevMenu, narration silent\n";

    // A briefing left with SHUTUP off opens the next time with it on.
    open_briefing("as the briefing opens again");
    exercise_click("SHUTUP");
    expect_briefing(kNarrationOff, "after SHUTUP is turned off again");
    exercise_click("PrevMenu");
    open_briefing("as the briefing opens after it was left silent");

    // SHUTUP stays on while the narration plays and turns itself off once it
    // is over, here ended as if it had played out.
    tick_mission_briefing();
    expect_briefing(kNarrationOn, "while the narration plays");
    audio_player_.stop_stream();
    SDL_Delay(2 * oa::ui::campaign::kTickerIntervalMs);
    tick_mission_briefing();
    expect_briefing(kNarrationOff, "once the narration is over");
    snapshot("over");

    // Escape is PrevMenu's key: it silences the narration and goes back.
    press(SDLK_ESCAPE);
    require(screen_ == Screen::new_campaign, "Escape did not go back to NEWGAME.GUI");
    require(!narrating(), "the narration plays on after Escape");
    std::cout << "briefing narration check: Escape, narration silent\n";

    // Start silences it and starts the mission.
    open_briefing("as the briefing opens for Start");
    exercise_click("Start");
    expect_mission("Start");
    snapshot("mission");

    // Enter is Start's key.
    leave_match();
    load(Screen::main_menu);
    open_new_campaign();
    open_briefing("as the briefing opens for Enter");
    press(SDLK_RETURN);
    expect_mission("Enter");
    std::cout << "briefing narration check: the narration stops whenever the briefing "
                 "silences it or is left\n";
}

} // namespace oa::app
