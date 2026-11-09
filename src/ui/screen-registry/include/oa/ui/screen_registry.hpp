// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Registration tables for frontend screens, overlays, dispatcher steps,
// dispatcher queries and kit screens.
//
// Packages describe their screens with plain descriptor structs and register
// them from one function listed in screens.inc, or from the extension's
// register_screens. The application owns the registry and hands packages a
// ScreenContext; packages never see Runtime. A kit screen
// (screen_registry/kit.hpp) is laid out with the OA UI kit and hosted on the
// OA layer, which opens it by name (ScreenServices::open_kit_screen).
#pragma once

#include "oa/ui/screen_registry/kit.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oa {
class AssetStore;
}

namespace oa::ui::frontend_renderer {
struct Surface;
}

namespace oa::sim::match_runtime {
class Match;
}

namespace oa::ui::frontend_state {
enum class Step : uint32_t;
enum class Query : uint32_t;
} // namespace oa::ui::frontend_state

namespace oa::app {

// Built-in screens use the values of app::Screen; package screens take ids
// from kFirstPackageScreen upward. Duplicate ids are rejected at startup.
using ScreenId = uint16_t;
constexpr ScreenId kFirstPackageScreen = 0x100;
constexpr ScreenId kScreenAny = 0xffff;

constexpr uint32_t kMaxScreens = 64;
constexpr uint32_t kMaxOverlays = 32;
constexpr uint32_t kMaxSteps = 64;
constexpr uint32_t kMaxQueries = 32;

enum class ScreenInputKind : uint8_t {
    key_down,
    key_up,
    text,
    pointer_move,
    pointer_down,
    pointer_up,
    wheel
};

// One platform input event. Pointer coordinates are in render space (the
// 640x480 canvas on frontend screens).
struct ScreenInput {
    ScreenInputKind kind{};
    uint8_t button{};     // pointer button, 1 = left
    uint8_t clicks{};     // pointer_down: presses in a row (SDL's click count)
    uint16_t modifiers{}; // SDL_Keymod bits
    uint32_t key{};       // SDL_Keycode for key events
    float x{};
    float y{};
    float wheel_y{};
    const char* text{}; // UTF-8 for text events, otherwise null
};

/// ScreenInput::modifiers bits of either Ctrl key.
constexpr uint16_t kInputModifierCtrl = 0x00c0;
/// ScreenInput::modifiers bits of either Alt key.
constexpr uint16_t kInputModifierAlt = 0x0300;
/// ScreenInput::modifiers bits of either system key (Command, Windows).
constexpr uint16_t kInputModifierSystem = 0x0c00;

// Application services available to packages. `host` is opaque.
struct ScreenServices {
    void (*request_screen)(void* host, ScreenId id){};
    // Plays the sound registered under `name` (allsound.tdf or
    // register_sound), else the WAV sounds/<name>.wav.
    void (*play_sound)(void* host, const char* name){};
    int (*read_number)(void* host, const char* section, const char* key, uint32_t* value){};
    void (*write_number)(void* host, const char* section, const char* key, uint32_t value){};
    // Reads the setting `key` of `section` into `out`, with its terminating
    // zero, when it is stored and fits in `capacity` bytes; returns 1 then,
    // and 0 otherwise. A null section reads `key` as a whole preference key,
    // as a Settings page's row binds one (kit::PreferenceRow::key), such as
    // "kit.example". Read at once.
    int (*read_string)(
        void* host, const char* section, const char* key, char* out, std::size_t capacity
    ){};
    void (*write_string)(void* host, const char* section, const char* key, const char* value){};
    void (*set_status)(void* host, const char* text){};
    // Writes both frontend signal bytes (the current and the pending signal).
    void (*set_frontend_signal)(void* host, uint8_t signal){};
    // Writes the frontend dispatcher's state byte.
    void (*set_frontend_state)(void* host, uint8_t state){};
    // Takes the main-menu panel (MAINMENU.GUI) off the screen.
    void (*close_main_menu_panel)(void* host){};
    // Loads MAINMENU.GUI again and runs its setup, sparks included.
    void (*reload_main_menu_panel)(void* host){};
    // Starts (nonzero) or stops platform text input; typed characters then
    // arrive as text events.
    void (*set_text_input)(void* host, int enabled){};
    // The frontend clock in game ticks, 30 per second.
    uint32_t (*current_tick)(void* host){};
    // Registers the sounds/ WAV `file` under the sound name `category`
    // unless the name is taken; play_sound then plays it by that name.
    void (*register_sound)(void* host, const char* category, const char* file){};
    // The pointer in frame pixels, where the software cursor is drawn.
    void (*pointer_position)(void* host, int32_t* x, int32_t* y){};
    // Reports (nonzero) that an overlay with buttons of its own over
    // MAINMENU.GUI's stands on the main menu; the main menu then takes the
    // layout made for that overlay instead of TA's own.
    void (*set_main_menu_overlay)(void* host, int present){};
    // Ends the run after the current event or frame, as the exit
    // confirmation's first choice does: a running match is left first, the
    // main loop stops, the extension's shutdown runs, the preferences are
    // written and oa-game exits with `exit_code`. `reason`, when neither null
    // nor empty, is shown first in a message box over the window, or written
    // to standard error without one (read at once).
    void (*quit)(void* host, const char* reason, int exit_code){};
    // Stops every sound that plays: the interface and unit sounds, the one
    // stream and the sound on the alternate route (the menu music). The CD
    // and track music go on.
    void (*stop_sounds)(void* host){};
    // Plays the sound registered under `name`, compared case-insensitively,
    // on the alternate route, which streams and repeats it as the menu music
    // plays, in place of whatever that route plays; nothing when sound is off
    // or the name is not registered.
    void (*play_sound_alternate)(void* host, const char* name){};
    // Runs one pass of the frontend dispatcher, as a pointer press does, once
    // the current event or frame has been handled; requests before then run
    // one pass, and none runs while a match is on screen.
    void (*run_frontend)(void* host){};
    /// Opens a kit screen registered under a name (kit_screen_register) on
    /// the OA layer, over the front end: modal, over a backdrop, laid out by
    /// its lay_out for the layer's size class and scale, and focused,
    /// reached and answered by the kit, as OA's own screens are. Called on
    /// the thread that runs main().
    ///
    /// @param host ScreenContext::host
    /// @param name the screen's name; read at once
    /// @return 1 when the screen was opened; 0 for a null name or one no kit
    ///         screen has
    int (*open_kit_screen)(void* host, const char* name){};
    /// Queues a notice on the OA layer's one queue of notices and prompts,
    /// the engine's and every extension's, which shows one at a time: the
    /// notice shows when nothing before it is shown, over the menus, and
    /// closes with its OK. Called on the thread that runs main().
    ///
    /// @param host ScreenContext::host
    /// @param request the notice; it and the strings it names are copied at
    ///        once
    /// @return the request's id, never 0; 0 when it is refused: a null
    ///         request, title or paragraph
    uint32_t (*show_notice)(void* host, const NoticeRequest* request){};
    /// Queues a prompt on the OA layer's one queue of notices and prompts,
    /// as show_notice queues a notice. Its answered is called once, on the
    /// thread that runs main(), with the button pressed, from 0 (Escape
    /// presses cancel_button), or with -1 when the runtime ends with the
    /// prompt still queued or shown. Called on the thread that runs main().
    ///
    /// @param host ScreenContext::host
    /// @param request the prompt; it and the strings it names are copied at
    ///        once, and its user is handed to answered
    /// @return the request's id, never 0; 0 when it is refused: a null
    ///         request, title, paragraph or caption, no button or more than
    ///         most_prompt_buttons, a button id that is not one word of a to
    ///         z, 0 to 9 and hyphens, or a cancel or primary button that is
    ///         neither -1 nor one of the buttons
    uint32_t (*ask_prompt)(void* host, const PromptRequest* request){};
    /// Tells whether the host's main-menu order owns this key.
    ///
    /// The main-menu overlay calls it before its own windows see a key.
    /// Nonzero means the host owns it: the overlay drops its focus and
    /// passes the key on. Zero, and a null function, leave the key with
    /// the overlay. The host owns the order's next and previous keys on
    /// the main menu, and, while its ring is showing on a landed stop,
    /// the arrows and the bound Accept and Press as well. A message box
    /// the overlay is showing does not ask.
    ///
    /// @param host ScreenContext::host
    /// @param input the key; read at once
    /// @return nonzero when the host owns the key
    int (*main_menu_order_owns_key)(void* host, const ScreenInput* input){};
};

// What screens, overlays and dispatcher steps are called with. host and
// services stay the same while the runtime lives; a package may keep them
// and use the services outside its callbacks, on the thread that runs
// main().
struct ScreenContext {
    void* host{};
    const ScreenServices* services{};
    oa::AssetStore* assets{};
    oa::ui::frontend_renderer::Surface* surface{}; // current frame
    oa::sim::match_runtime::Match* world{};        // null outside a match
    const ScreenInput* input{};                    // set only during event callbacks
    ScreenId screen{};                             // screen currently shown
};

using ScreenFn = void (*)(ScreenContext* ctx, void* state);
using ScreenEventFn = int (*)(ScreenContext* ctx, void* state); // nonzero = consumed
using ScreenBackgroundFn = const char* (*)(ScreenContext* ctx, void* state);

/// What a control on a window a screen package shows is.
enum class ScreenWindowKind : uint8_t {
    button,     ///< a push button
    check_box,  ///< a button that flips between checked and not
    list,       ///< a list of rows
    text_field, ///< a field that takes typed text
    slider,     ///< a scroll bar
    label,      ///< text that takes no pointer
    area,       ///< a surface that takes the pointer
    image,      ///< a picture that takes no pointer
};

/// One control of a window a screen package shows.
struct ScreenWindowControl {
    std::string name;               ///< as the package spells it; empty is left out
    ScreenWindowKind kind{};        ///< what it is
    int32_t x{};                    ///< its left column on the canvas
    int32_t y{};                    ///< its top row on the canvas
    int32_t width{};                ///< its width in canvas pixels
    int32_t height{};               ///< its height in canvas pixels
    bool visible{true};             ///< it is shown
    bool enabled{true};             ///< it takes a click
    bool focused{};                 ///< it holds the keyboard focus
    bool checked{};                 ///< a check box is checked
    std::string text;               ///< its caption, its label's text or a field's typed text
    std::vector<std::string> items; ///< a list's rows
    int32_t first_visible{};        ///< a list's first row shown
    int32_t rows{};                 ///< the rows a list shows at once
    int32_t row_height{};           ///< a list's row pitch in canvas pixels
    int32_t selected{-1};           ///< a list's selected row; -1 for none
};

/// One window a screen package shows, and the controls on it.
struct ScreenPackageWindow {
    std::string name;                          ///< as the package spells it; empty is left out
    std::vector<ScreenWindowControl> controls; ///< its controls, in the package's order
};

// GUI resources loaded before `enter`. A null layout means `enter` supplies
// its own resources.
struct ScreenAssets {
    const char* layout{};
    const char* background{}; // named background (bitmaps\<name>.pcx); null for none
    const char* palette{};
    const char* sprites{};
    const char* shared_sprites{};
};

struct ScreenDesc {
    ScreenId id{};
    const char* name{};
    ScreenAssets assets;
    ScreenBackgroundFn background; // optional; overrides assets.background
    ScreenFn enter;                // after resources load
    ScreenFn leave;                // before the next screen loads
    ScreenEventFn event;           // after overlays, before built-in handling
    ScreenFn tick;                 // once per frame
    ScreenFn draw;                 // after the GUI is composed, before the cursor
    void* state{};
    /// Lists the windows this package shows, for a program driving the game.
    ///
    /// Null reports none, as a package that draws no windows of its own
    /// does. Called on the thread that runs main(), while the controls are
    /// listed, with a context that lives only for the call. The vector is
    /// empty on entry. The function must not change the game and must not
    /// throw.
    ///
    /// @param ctx the package context, valid for the call
    /// @param state ScreenDesc::state
    /// @param[out] windows receives the windows; empty on entry
    void (*windows)(ScreenContext* ctx, void* state, std::vector<ScreenPackageWindow>* windows){};
};

// Drawn over every screen matching `screen` (or kScreenAny) in ascending z;
// input reaches overlays in descending z, before the screen itself.
struct OverlayDesc {
    const char* name{};
    ScreenId screen{};
    int16_t z{};
    ScreenFn create; // once, after all registrations
    ScreenEventFn event;
    ScreenFn tick;
    ScreenFn draw;
    void* state{};
};

struct StepDesc {
    oa::ui::frontend_state::Step step{};
    ScreenFn run;
    void* state{};
};

// Answers a dispatcher query; its result is the query's.
using ScreenQueryFn = uint32_t (*)(ScreenContext* ctx, void* state);

struct QueryDesc {
    oa::ui::frontend_state::Query query{};
    ScreenQueryFn run{};
    void* state{};
};

struct ScreenRegistry {
    ScreenDesc screens[kMaxScreens];
    uint32_t screen_count{};
    OverlayDesc overlays[kMaxOverlays]; // kept sorted by z
    uint32_t overlay_count{};
    StepDesc steps[kMaxSteps];
    uint32_t step_count{};
    QueryDesc queries[kMaxQueries]{};
    uint32_t query_count{};
    const char* rejected{}; // name of the first rejected registration
    /// The kit screens registered (kit_screen_register), in registration
    /// order, which ScreenServices::open_kit_screen opens by name.
    KitScreenDesc kit_screens[kMaxKitScreens]{};
    /// How many kit screens are registered.
    uint32_t kit_screen_count{};
};

/// Adds a screen to the registry.
///
/// Returns false and records the name in `rejected` for kScreenAny, a
/// duplicate id or a full table.
///
/// @param[in,out] registry registry to add to
/// @param desc screen description, copied
/// @return true when registered
bool screen_register(ScreenRegistry* registry, const ScreenDesc* desc);

/// Adds an overlay to the registry, keeping the overlays sorted by z.
///
/// An overlay goes after those of equal z. Returns false and records the name
/// in `rejected` when the table is full.
///
/// @param[in,out] registry registry to add to
/// @param desc overlay description, copied
/// @return true when registered
bool overlay_register(ScreenRegistry* registry, const OverlayDesc* desc);

/// Binds a handler to a dispatcher step.
///
/// Returns false and records "dispatcher step" in `rejected` for a null
/// handler, a step already bound or a full table.
///
/// @param[in,out] registry registry to add to
/// @param step dispatcher step
/// @param run handler
/// @param state handler state passed back to `run`
/// @return true when registered
bool step_register(
    ScreenRegistry* registry, oa::ui::frontend_state::Step step, ScreenFn run, void* state
);

/// Binds a handler to a dispatcher query.
///
/// Returns false and records "dispatcher query" in `rejected` for a null
/// handler, a query already bound or a full table. A query no handler takes
/// is answered with 0.
///
/// @param[in,out] registry registry to add to
/// @param query dispatcher query
/// @param run handler
/// @param state handler state passed back to `run`
/// @return true when registered
bool query_register(
    ScreenRegistry* registry, oa::ui::frontend_state::Query query, ScreenQueryFn run, void* state
);

/// Adds a kit screen to the registry, which ScreenServices::open_kit_screen
/// then opens by its name on the OA layer.
///
/// The description is copied; its name and model, and what they name, must
/// live as long as the runtime. Returns false and records the name in
/// `rejected` ("(unnamed)" for a null or empty one) for a name that is not
/// dotted words of a to z, 0 to 9 and hyphens of at most 48 bytes, a name
/// another kit screen has, a screen with no lay_out or act, or a full table.
///
/// @param[in,out] registry registry to add to; null takes nothing
/// @param screen the kit screen's description, copied
/// @return true when registered
bool kit_screen_register(ScreenRegistry* registry, const KitScreenDesc& screen);

/// Finds a registered screen.
///
/// @param registry registry to search
/// @param id screen id
/// @return the screen's description, or null when none has the id
[[nodiscard]] const ScreenDesc* screen_find(const ScreenRegistry* registry, ScreenId id);

/// Finds the handler bound to a dispatcher step.
///
/// @param registry registry to search
/// @param step dispatcher step
/// @return the binding, or null when the step has none
[[nodiscard]] const StepDesc*
step_find(const ScreenRegistry* registry, oa::ui::frontend_state::Step step);

/// Finds the handler bound to a dispatcher query.
///
/// @param registry registry to search
/// @param query dispatcher query
/// @return the binding, or null when the query has none
[[nodiscard]] const QueryDesc*
query_find(const ScreenRegistry* registry, oa::ui::frontend_state::Query query);

/// Asks the host to show a screen.
///
/// @param ctx package context
/// @param id screen to show
inline void screen_request(ScreenContext* ctx, ScreenId id) {
    ctx->services->request_screen(ctx->host, id);
}

/// Plays a named interface sound through the host.
///
/// @param ctx package context
/// @param name sound name
inline void screen_play_sound(ScreenContext* ctx, const char* name) {
    ctx->services->play_sound(ctx->host, name);
}

/// Sets the host's status line.
///
/// @param ctx package context
/// @param text status text
inline void screen_status(ScreenContext* ctx, const char* text) {
    ctx->services->set_status(ctx->host, text);
}

// Declares every package registration function listed in screens.inc.
#define OA_REGISTER(fn) void fn(ScreenRegistry* registry);
#include "oa/ui/screen_registry/screens.inc"
#undef OA_REGISTER

} // namespace oa::app
