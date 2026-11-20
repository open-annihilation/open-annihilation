// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The check host (check_host.hpp): each entry is one of the runtime's own
// steps, the ones the engine's checks drive the game through.
#include "oa/app/check_host.hpp"

#include "check_host_input.hpp"
#include "oa/app/runtime.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace oa::app {

// The runtime's side of the check host: the one friend through which its
// entries reach the runtime.
struct CheckHostAccess {
    /// Returns the runtime a check host's context names.
    ///
    /// @param context CheckHost::context
    /// @return the runtime
    static Runtime& runtime(void* context) { return *static_cast<Runtime*>(context); }

    /// Hands one SDL event to the runtime (CheckHost::dispatch).
    ///
    /// @param context CheckHost::context
    /// @param[in,out] event the event
    /// @return false when the event ended the run
    static bool dispatch(void* context, SDL_Event* event) {
        bool running = true;
        runtime(context).dispatch_event(*event, running);
        return running;
    }

    /// Hands the runtime a pointer event at a canvas point (CheckHost::pointer).
    ///
    /// @param context CheckHost::context
    /// @param type the pointer event's SDL type
    /// @param x canvas column
    /// @param y canvas row
    /// @param clicks the presses in a row a press or release counts
    /// @return false when the event ended the run
    static bool pointer(void* context, uint32_t type, int32_t x, int32_t y, uint8_t clicks) {
        auto& game = runtime(context);
        SDL_Event event = check_host_input::pointer_event(
            game.sdl_.renderer, game.sdl_.window, type, x, y, clicks
        );
        return dispatch(context, &event);
    }

    /// Reads the cursor's position (CheckHost::cursor).
    ///
    /// @param context CheckHost::context
    /// @param[out] x canvas column
    /// @param[out] y canvas row
    static void cursor(void* context, int32_t* x, int32_t* y) {
        const auto& game = runtime(context);
        *x = static_cast<int32_t>(game.pointer_x_);
        *y = static_cast<int32_t>(game.pointer_y_);
    }

    /// Returns the window's SDL id (CheckHost::window_id).
    ///
    /// @param context CheckHost::context
    /// @return the id; 0 without a window
    static uint32_t window_id(void* context) {
        auto* window = runtime(context).sdl_.window;
        return window != nullptr ? SDL_GetWindowID(window) : 0;
    }

    /// Runs one frame of the main loop (CheckHost::frame).
    ///
    /// @param context CheckHost::context
    static void frame(void* context) {
        auto& game = runtime(context);
        if (game.sdl_.renderer == nullptr)
            throw std::runtime_error("check host: a frame needs the game's window");
        game.idle_tick();
    }

    /// Ticks the screen packages and composes the frame (CheckHost::package_pass).
    ///
    /// @param context CheckHost::context
    static void package_pass(void* context) {
        auto& game = runtime(context);
        game.tick_screen_packages();
        game.rebuild_surface();
    }

    /// Composes the frame (CheckHost::compose).
    ///
    /// @param context CheckHost::context
    static void compose(void* context) { runtime(context).rebuild_surface(); }

    /// Returns the frame last composed (CheckHost::surface).
    ///
    /// @param context CheckHost::context
    /// @return the frame
    static const renderer::Surface* surface(void* context) { return &runtime(context).surface_; }

    /// Holds the frontend clock (CheckHost::hold_clock).
    ///
    /// @param context CheckHost::context
    /// @param tick the clock's value, in game ticks
    static void hold_clock(void* context, uint32_t tick) {
        runtime(context).fake_frontend_tick_ = tick;
    }

    /// Lets the frontend clock follow real time (CheckHost::release_clock).
    ///
    /// @param context CheckHost::context
    static void release_clock(void* context) { runtime(context).fake_frontend_tick_.reset(); }

    /// Returns the frontend clock (CheckHost::clock).
    ///
    /// @param context CheckHost::context
    /// @return the clock in game ticks
    static uint32_t clock(void* context) { return runtime(context).frontend_tick(); }

    /// Returns the screen shown (CheckHost::screen).
    ///
    /// @param context CheckHost::context
    /// @return its id
    static ScreenId screen(void* context) { return screen_id(runtime(context).screen_); }

    /// Returns the frontend dispatcher's state (CheckHost::frontend_state).
    ///
    /// @param context CheckHost::context
    /// @return its state byte
    static uint8_t frontend_state(void* context) { return runtime(context).state_.state; }

    /// Tells whether a package owns the main menu's frame (CheckHost::frame_owned_by_package).
    ///
    /// @param context CheckHost::context
    /// @return true while one does
    static bool frame_owned_by_package(void* context) {
        return runtime(context).frame_owned_by_package();
    }

    /// Finds a gadget of the layout shown (CheckHost::gadget).
    ///
    /// @param context CheckHost::context
    /// @param name the gadget's name
    /// @param[out] x its left column
    /// @param[out] y its top row
    /// @param[out] width its width
    /// @param[out] height its height
    /// @return true when the layout holds it
    static bool gadget(
        void* context, const char* name, int32_t* x, int32_t* y, int32_t* width, int32_t* height
    ) {
        if (name == nullptr)
            return false;
        const auto* found = check_host_input::find_gadget(runtime(context).resources_.layout, name);
        if (found == nullptr)
            return false;
        *x = found->common.x;
        *y = found->common.y;
        *width = found->common.width;
        *height = found->common.height;
        return true;
    }

    /// Returns the game's files (CheckHost::assets).
    ///
    /// @param context CheckHost::context
    /// @return the store
    static const oa::AssetStore* assets(void* context) { return &runtime(context).assets_; }

    /// Finds the file a sound name plays (CheckHost::sound_resource).
    ///
    /// @param context CheckHost::context
    /// @param name the sound name
    /// @param[out] out receives the file's path and a zero byte
    /// @param size the bytes `out` holds
    /// @return false when they do not fit or `name` is null
    static bool sound_resource(void* context, const char* name, char* out, std::size_t size) {
        if (name == nullptr)
            return false;
        return check_host_input::copy_text(runtime(context).screen_sound_resource(name), out, size);
    }

    /// Writes the preferences file (CheckHost::write_preferences).
    ///
    /// @param context CheckHost::context
    static void write_preferences(void* context) { runtime(context).flush_preferences(); }

    /// Returns the runtime's options (runtime_options).
    ///
    /// @param game the runtime
    /// @return its options
    static const Options& options(const Runtime& game) { return game.options_; }

    /// Writes the shown window, kit screens included. The console surface
    /// does not hold a notice drawn over it. The extension sees Runtime
    /// incomplete.
    static void snapshot_stage(Runtime& game, const char* stage) {
        if (stage != nullptr)
            game.write_presented_snapshot(stage);
    }

    /// Asks the Library to open on the battle just played.
    static void queue_battle_share(Runtime& game) { game.open_replays_to_share_battle(); }
};

namespace {

// Every entry of the check host; check_host() binds a copy to a runtime.
constexpr CheckHost kCheckHostEntries{
    nullptr,
    CheckHostAccess::dispatch,
    CheckHostAccess::pointer,
    CheckHostAccess::cursor,
    CheckHostAccess::window_id,
    CheckHostAccess::frame,
    CheckHostAccess::package_pass,
    CheckHostAccess::compose,
    CheckHostAccess::surface,
    CheckHostAccess::hold_clock,
    CheckHostAccess::release_clock,
    CheckHostAccess::clock,
    CheckHostAccess::screen,
    CheckHostAccess::frontend_state,
    CheckHostAccess::frame_owned_by_package,
    CheckHostAccess::gadget,
    CheckHostAccess::assets,
    CheckHostAccess::sound_resource,
    CheckHostAccess::write_preferences
};

// The entries above, one for each of the table's after its context.
constexpr std::size_t kCheckHostEntryCount = 18;
static_assert(
    sizeof(CheckHost) == sizeof(void*) * (1 + kCheckHostEntryCount),
    "the check host's entries changed: set every one of them here"
);

} // namespace

CheckHost check_host(Runtime& runtime) {
    CheckHost host = kCheckHostEntries;
    host.context = &runtime;
    return host;
}

const Options& runtime_options(const Runtime& runtime) {
    return CheckHostAccess::options(runtime);
}

void snapshot_presented_stage(Runtime& runtime, const char* stage) {
    CheckHostAccess::snapshot_stage(runtime, stage);
}

void queue_battle_replay_share(Runtime& runtime) {
    CheckHostAccess::queue_battle_share(runtime);
}

bool battle_has_recording(const Runtime& runtime) {
    return runtime.battle_has_recording();
}

} // namespace oa::app
