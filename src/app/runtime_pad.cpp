// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The gamepad dispatcher: gamepads opened and closed, their events taken
// and turned into the engine's own actions, the F13–F16 keys of Steam
// Input's grips while a gamepad is open, the pads' frame, whether the pad
// layer is on and the settings the pad reads (docs/controllers.md).
#include "oa/app/runtime.hpp"
#include "engine_settings_state.hpp"
#include "pad_state.hpp"
#include "touch_state.hpp"
#include "oa/sim/gameplay_input/input.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/pad_controls.hpp"
#include "oa/ui/touch_hud.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <optional>

namespace oa::app {
namespace {

namespace pc = oa::ui::pad_controls;
namespace hud = oa::ui::touch_hud;
namespace settings = oa::ui::engine_settings;

/// Nanoseconds in a millisecond.
constexpr uint64_t kNanosecondsPerMillisecond = 1'000'000;
/// The longest step the pad's frame takes, nanoseconds: after a stalled frame the sticks and
/// timers move on as after this long, so the camera never jumps.
constexpr uint64_t kLongestPadStepNs = 100 * kNanosecondsPerMillisecond;
/// An axis's reading at full deflection or pull (SDL's axes run to ±32767).
constexpr float kAxisFull = 32767.0F;
/// The hexadecimal digits of a USB vendor or product id, as the log writes them.
constexpr int kUsbIdDigits = 4;

/// Holds the run loop's flag in the pad state for one event, so that synthetic events the event
/// sends end the run through it; restores the one before, whatever happens.
class RunningScope {
  public:

    /// Sets the flag.
    ///
    /// @param[in,out] slot the pad state's running flag (Runtime::PadState::running)
    /// @param[in,out] running the event's running flag
    RunningScope(bool*& slot, bool& running) noexcept : slot_(slot), before_(slot) {
        slot_ = &running;
    }

    /// Restores the flag before.
    ~RunningScope() { slot_ = before_; }

    RunningScope(const RunningScope&) = delete;
    RunningScope& operator=(const RunningScope&) = delete;

  private:

    bool*& slot_;    ///< the pad state's running flag
    bool* before_{}; ///< the flag held before
};

/// Returns the type the controls use for SDL's reading of a pad.
///
/// @param type SDL's gamepad type
/// @return the family
pc::PadType reported_type(SDL_GamepadType type) noexcept {
    switch (type) {
    case SDL_GAMEPAD_TYPE_STANDARD:
        return pc::PadType::standard;
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
        return pc::PadType::xbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return pc::PadType::playstation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#if SDL_VERSION_ATLEAST(3, 4, 0)
    case SDL_GAMEPAD_TYPE_GAMECUBE:
#endif
        return pc::PadType::nintendo;
    default:
        return pc::PadType::unknown;
    }
}

/// Returns the physical button a gamepad button is. The Steam Deck's own controls report the
/// left trackpad's press as the touchpad button, the right one's as MISC2 and the sticks'
/// touches as MISC3 (left) and MISC4 (right); on other pads those buttons mean other things and
/// are left alone, as are Guide and MISC1 (the system's).
///
/// @param button SDL's gamepad button
/// @param type the pad's type
/// @return the physical button, or none for a button the controls do not read
std::optional<pc::PadButton> button_of(uint8_t button, pc::PadType type) noexcept {
    const bool deck = type == pc::PadType::steam_deck;
    switch (static_cast<SDL_GamepadButton>(button)) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return pc::PadButton::a;
    case SDL_GAMEPAD_BUTTON_EAST:
        return pc::PadButton::b;
    case SDL_GAMEPAD_BUTTON_WEST:
        return pc::PadButton::x;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return pc::PadButton::y;
    case SDL_GAMEPAD_BUTTON_BACK:
        return pc::PadButton::view;
    case SDL_GAMEPAD_BUTTON_START:
        return pc::PadButton::menu;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return pc::PadButton::l3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return pc::PadButton::r3;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return pc::PadButton::l1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return pc::PadButton::r1;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return pc::PadButton::dpad_up;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return pc::PadButton::dpad_right;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return pc::PadButton::dpad_down;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return pc::PadButton::dpad_left;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1:
        return pc::PadButton::r4;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1:
        return pc::PadButton::l4;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2:
        return pc::PadButton::r5;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2:
        return pc::PadButton::l5;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:
        return pc::PadButton::left_pad;
    case SDL_GAMEPAD_BUTTON_MISC2:
        return deck ? std::optional{pc::PadButton::right_pad} : std::nullopt;
    case SDL_GAMEPAD_BUTTON_MISC3:
        return deck ? std::optional{pc::PadButton::left_stick_touch} : std::nullopt;
    case SDL_GAMEPAD_BUTTON_MISC4:
        return deck ? std::optional{pc::PadButton::right_stick_touch} : std::nullopt;
    default:
        return std::nullopt;
    }
}

/// Returns the grip a key of Open Annihilation's Steam Input layout stands for.
///
/// @param key the key
/// @return R4 for F13, R5 for F14, L4 for F15, L5 for F16; none for another key
std::optional<pc::PadButton> grip_of_key(SDL_Keycode key) noexcept {
    if (key == grip_key_r4)
        return pc::PadButton::r4;
    if (key == grip_key_r5)
        return pc::PadButton::r5;
    if (key == grip_key_l4)
        return pc::PadButton::l4;
    if (key == grip_key_l5)
        return pc::PadButton::l5;
    return std::nullopt;
}

/// Returns an axis reading as a fraction of full deflection.
///
/// @param value SDL's reading, -32768..32767
/// @return -1..1
float axis_fraction(int16_t value) noexcept {
    return std::clamp(static_cast<float>(value) / kAxisFull, -1.0F, 1.0F);
}

/// Returns the entry of an open pad.
///
/// @param pads the open pads (Runtime::PadState::pads)
/// @param id the pad's joystick
/// @return the entry, or null when that pad is not open
OpenPad* open_pad_of(std::array<OpenPad, max_open_pads>& pads, SDL_JoystickID id) noexcept {
    if (id == 0)
        return nullptr;
    for (auto& pad : pads)
        if (pad.id == id)
            return &pad;
    return nullptr;
}

} // namespace

void Runtime::destroy_pad_state(PadState* state) noexcept {
    delete state;
}

Runtime::PadState& Runtime::pad_state() {
    if (!pad_)
        pad_.reset(new PadState());
    return *pad_;
}

const Runtime::PadState* Runtime::pad_state_if_made() const {
    return pad_.get();
}

bool Runtime::take_pad_event(SDL_Event& event, bool& running) {
    // A mouse, trackpad or pen of its own takes the pointer back from the
    // gamepads. The pad pointer's own events and a finger's do not.
    if (pad_ &&
        (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)) {
        const SDL_MouseID which =
            event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.which : event.button.which;
        if (which != pad_mouse_id && which != SDL_TOUCH_MOUSEID)
            pad_->ahead_of_mouse = false;
    }
    // The joystick events SDL sends beside the gamepad events arrive only
    // because the gamepad subsystem runs; no screen ever saw them.
    if (event.type >= SDL_EVENT_JOYSTICK_AXIS_MOTION &&
        event.type <= SDL_EVENT_JOYSTICK_UPDATE_COMPLETE)
        return true;
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        PadAccess::open_pad(*this, event.gdevice.which);
        return true;
    case SDL_EVENT_GAMEPAD_REMOVED: {
        auto& state = pad_state();
        const RunningScope scope(state.running, running);
        PadAccess::close_pad(*this, event.gdevice.which);
        return true;
    }
    case SDL_EVENT_GAMEPAD_REMAPPED:
    case SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED:
        if (auto* pad = open_pad_of(pad_state().pads, event.gdevice.which); pad != nullptr)
            PadAccess::read_traits(*pad);
        return true;
    case SDL_EVENT_GAMEPAD_UPDATE_COMPLETE:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        auto& state = pad_state();
        const auto* pad = open_pad_of(state.pads, event.gbutton.which);
        if (pad == nullptr)
            return true;
        const auto button = button_of(event.gbutton.button, pad->traits.type);
        if (!button)
            return true;
        const RunningScope scope(state.running, running);
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (down)
            PadAccess::note_input(*this, event.gbutton.which);
        PadAccess::take_button(*this, *button, down);
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        auto& state = pad_state();
        if (open_pad_of(state.pads, event.gaxis.which) == nullptr)
            return true;
        const RunningScope scope(state.running, running);
        const float value = axis_fraction(event.gaxis.value);
        // A resting stick of a pad not in use leaves the one in use alone.
        const bool active = state.active == event.gaxis.which;
        const auto take_stick = [&](pc::Side side, bool across) {
            pc::Vec2 stick = state.sticks[static_cast<std::size_t>(side)];
            (across ? stick.x : stick.y) = value;
            if (pc::stick_deflection(stick) > 0.0F)
                PadAccess::note_input(*this, event.gaxis.which);
            else if (!active)
                return;
            state.sticks[static_cast<std::size_t>(side)] = stick;
        };
        const auto take_trigger = [&](pc::TriggerButton& trigger, pc::PadButton button) {
            const bool before = trigger.down();
            const bool after = trigger.update(std::max(value, 0.0F));
            if (after == before)
                return;
            if (after)
                PadAccess::note_input(*this, event.gaxis.which);
            PadAccess::take_button(*this, button, after);
        };
        switch (static_cast<SDL_GamepadAxis>(event.gaxis.axis)) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            take_stick(pc::Side::left, true);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            take_stick(pc::Side::left, false);
            break;
        case SDL_GAMEPAD_AXIS_RIGHTX:
            take_stick(pc::Side::right, true);
            break;
        case SDL_GAMEPAD_AXIS_RIGHTY:
            take_stick(pc::Side::right, false);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
            take_trigger(state.left_trigger, pc::PadButton::l2);
            break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
            take_trigger(state.right_trigger, pc::PadButton::r2);
            break;
        default:
            break;
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP: {
        auto& state = pad_state();
        if (open_pad_of(state.pads, event.gtouchpad.which) == nullptr)
            return true;
        const RunningScope scope(state.running, running);
        if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN)
            PadAccess::note_input(*this, event.gtouchpad.which);
        else if (state.active != event.gtouchpad.which)
            return true;
        PadAccess::take_touchpad(*this, event.gtouchpad);
        return true;
    }
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
        // A gyro's readings move the pointer but turn nothing on: they come
        // while the pad lies still.
        auto& state = pad_state();
        if (state.active != event.gsensor.which || open_pad_of(state.pads, state.active) == nullptr)
            return true;
        const RunningScope scope(state.running, running);
        PadAccess::take_sensor(*this, event.gsensor);
        return true;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        // Only while a gamepad is open are F13–F16 the grips, so a keyboard
        // with those keys is unchanged without one.
        if (!PadAccess::any_pad_open(*this))
            return false;
        const auto grip = grip_of_key(event.key.key);
        if (!grip)
            return false;
        auto& state = pad_state();
        state.grip_keys_seen = true;
        if (event.key.repeat)
            return true;
        const RunningScope scope(state.running, running);
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down)
            PadAccess::note_input(*this, 0);
        PadAccess::take_button(*this, *grip, down);
        return true;
    }
    default:
        return false;
    }
}

void Runtime::tick_pad() {
    if (!PadAccess::any_pad_open(*this))
        PadAccess::open_present_pads(*this);
    // A desktop that never had a gamepad has no pad state.
    if (!pad_)
        return;
    auto& state = *pad_;
    const uint64_t now = PadAccess::now_ns(*this);
    const uint64_t elapsed = state.tick_ns != 0 && now > state.tick_ns
                                 ? std::min(now - state.tick_ns, kLongestPadStepNs)
                                 : 0;
    state.tick_ns = now;
    const auto chosen = pad_settings();
    PadAccess::sync_sensors(*this, chosen);
    // Until a gamepad sends input nothing of the pad layer runs or draws.
    if (!pad_used())
        return;
    PadAccess::configure_pointers(*this, chosen);
    PadAccess::refresh_targets(*this);
    PadAccess::advance_holds(*this, now);
    PadAccess::tick_pointer(*this, chosen, now, elapsed);
    PadAccess::refresh_rings(*this);
    PadAccess::aim_ring(*this, now);
    // FORCE gives Ctrl to the pointer word the moment it changes, a finger
    // on the FORCE chip as well as the grip.
    if (const bool force = pad_force_held(); force != state.force_seen) {
        state.force_seen = force;
        refresh_pointer_modifiers();
    }
    // A placement the pad started ends with the placement.
    if (touch_ && touch_->dispatch.placement_by_pad && !PadAccess::placing(*this))
        touch_->dispatch.placement_by_pad = false;
    PadAccess::write_look(*this, chosen);
}

void Runtime::pad_screen_changed() {
    if (!pad_)
        return;
    PadAccess::forget_held(*this, false);
}

bool Runtime::pad_used() const {
    const PadState* state = pad_state_if_made();
    return state != nullptr && (state->forced || state->used);
}

bool Runtime::pad_force_held() const {
    if (const PadState* state = pad_state_if_made(); state != nullptr && state->force)
        return true;
    const auto* touch = touch_state_if_made();
    return touch != nullptr && touch->hud.force_touch;
}

bool Runtime::pad_steam_input() const {
    const auto* pad = PadAccess::active_pad(*this);
    return pad != nullptr && pad->traits.steam_input;
}

oa::ui::pad_controls::PadSettings Runtime::pad_settings() const {
    pc::PadSettings result{};
    if (!engine_settings_)
        return result;
    const auto& chosen = engine_settings_->current;
    result.scheme = chosen.pad_scheme;
    result.right_trackpad = chosen.pad_right_trackpad;
    result.pointer_speed =
        std::clamp(chosen.pad_pointer_speed, pc::lowest_pointer_speed, pc::highest_pointer_speed);
    result.acceleration = chosen.pad_acceleration;
    result.glide = chosen.pad_glide;
    result.right_stick = chosen.pad_right_stick;
    result.magnetism = chosen.pad_magnetism;
    result.gyro = chosen.pad_gyro;
    result.gyro_speed =
        std::clamp(chosen.pad_gyro_speed, pc::lowest_gyro_speed, pc::highest_gyro_speed);
    result.haptics = chosen.pad_haptics;
    result.prompts = chosen.pad_prompts;
    result.left_handed = chosen.pad_left_handed;
    result.hold_ms = std::clamp(
        chosen.touch_hold_ms, settings::lowest_touch_hold_ms, settings::highest_touch_hold_ms
    );
    return result;
}

uint64_t PadAccess::now_ns(const Runtime& runtime) {
    const Runtime::PadState* state = runtime.pad_state_if_made();
    if (state != nullptr && state->check_clock_ns)
        return *state->check_clock_ns;
    return SDL_GetTicksNS();
}

const OpenPad* PadAccess::active_pad(const Runtime& runtime) {
    const Runtime::PadState* state = runtime.pad_state_if_made();
    if (state == nullptr)
        return nullptr;
    const OpenPad* first = nullptr;
    for (const auto& pad : state->pads) {
        if (pad.id == 0)
            continue;
        if (pad.id == state->active)
            return &pad;
        if (first == nullptr)
            first = &pad;
    }
    return first;
}

pc::PadTraits PadAccess::traits(const Runtime& runtime) {
    const auto* pad = active_pad(runtime);
    return pad != nullptr ? pad->traits : pc::PadTraits{};
}

pc::MapContext PadAccess::map_context(const Runtime& runtime) {
    const auto chosen = runtime.pad_settings();
    const auto pad = traits(runtime);
    const Runtime::PadState* state = runtime.pad_state_if_made();
    pc::MapContext context{};
    context.scheme = pc::effective_scheme(chosen.scheme, pad);
    context.fallback = !(pad.grips || (state != nullptr && state->grip_keys_seen));
    context.trackpads = pad.trackpads >= pad_trackpad_count;
    context.left_handed = chosen.left_handed;
    return context;
}

bool PadAccess::in_match(Runtime& runtime) {
    // A question photo mode asks takes the pad as a menu does.
    return runtime.screen_ == Screen::match && runtime.match_ && !runtime.match_paused_ &&
           !runtime.match_finished_ && runtime.engine_settings_dialog() == nullptr &&
           !runtime.photo_asks();
}

pc::Held PadAccess::held(Runtime& runtime) {
    pc::Held result{};
    result.in_match = in_match(runtime);
    if (const auto* state = runtime.pad_state_if_made(); state != nullptr) {
        // A layer key takes effect the moment it is down.
        for (std::size_t index = 0; index < state->down.size(); ++index) {
            if (!state->down[index])
                continue;
            switch (state->pressed[index].action) {
            case pc::Action::groups_layer:
                result.groups = true;
                break;
            case pc::Action::game_layer:
                result.game = true;
                break;
            case pc::Action::standing_layer:
                result.standing = true;
                break;
            default:
                break;
            }
        }
    }
    if (const auto* touch = runtime.touch_state_if_made(); touch != nullptr) {
        result.order_ring = touch->hud.radial.has_value();
        result.build_ring = touch->hud.build_ring.has_value();
        result.select_sheet = touch->hud.sheet == hud::Sheet::select_menu;
    }
    // Photo mode wins even when the match is paused, so a pad cannot fall
    // through to the menus' clicks. The question it asks over itself is
    // answered as a menu's.
    result.photo = runtime.photo_mode() && !runtime.photo_asks();
    return result;
}

pc::Layer PadAccess::layer(Runtime& runtime) {
    return pc::layer_for(held(runtime));
}

bool PadAccess::any_pad_open(const Runtime& runtime) {
    const Runtime::PadState* state = runtime.pad_state_if_made();
    return state != nullptr &&
           std::any_of(state->pads.begin(), state->pads.end(), [](const OpenPad& pad) {
               return pad.id != 0;
           });
}

void PadAccess::open_pad(Runtime& runtime, SDL_JoystickID id) {
    auto& state = runtime.pad_state();
    if (open_pad_of(state.pads, id) != nullptr)
        return;
    const auto free = std::find_if(state.pads.begin(), state.pads.end(), [](const OpenPad& pad) {
        return pad.id == 0;
    });
    // Pads past the most kept open are left closed, and while the pad
    // check runs so is every pad but its virtual stand-ins: a gamepad the
    // machine has, such as a simulator's own, stays out of it.
    if (free == state.pads.end() || (state.virtual_pads_only && !SDL_IsJoystickVirtual(id)))
        return;
    SDL_Gamepad* gamepad = SDL_OpenGamepad(id);
    if (gamepad == nullptr) {
        std::cerr << "warning: a gamepad did not open: " << SDL_GetError() << '\n';
        return;
    }
    *free = OpenPad{};
    free->id = id;
    free->gamepad = gamepad;
    read_traits(*free);
    // The log names each pad opened, and whether Steam Input gives it or
    // SDL reads the controller itself.
    const char* name = SDL_GetGamepadName(gamepad);
    std::cout << "open-annihilation: gamepad opened: " << (name != nullptr ? name : "unnamed")
              << " (vendor 0x" << std::hex << std::setfill('0') << std::setw(kUsbIdDigits)
              << free->traits.vendor << ", product 0x" << std::setw(kUsbIdDigits)
              << free->traits.product << std::dec << std::setfill(' ') << ')'
              << (free->traits.steam_input ? ", through Steam Input" : "") << '\n';
    sync_sensors(runtime, runtime.pad_settings());
}

void PadAccess::open_present_pads(Runtime& runtime) {
    if (SDL_WasInit(SDL_INIT_GAMEPAD) != SDL_INIT_GAMEPAD || !SDL_HasGamepad())
        return;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids == nullptr)
        return;
    for (int index = 0; index < count; ++index)
        open_pad(runtime, ids[index]);
    SDL_free(ids);
}

void PadAccess::close_pad(Runtime& runtime, SDL_JoystickID id) {
    auto& state = runtime.pad_state();
    auto* pad = open_pad_of(state.pads, id);
    if (pad == nullptr)
        return;
    if (pad->gamepad != nullptr)
        SDL_CloseGamepad(pad->gamepad);
    *pad = OpenPad{};
    // A pad that goes away lets go of what it held; another open pad is
    // in use from its next input.
    if (state.active == id) {
        state.active = 0;
        forget_held(runtime, true);
    }
}

void PadAccess::read_traits(OpenPad& pad) {
    SDL_Gamepad* gamepad = pad.gamepad;
    if (gamepad == nullptr)
        return;
    auto& traits = pad.traits;
    traits.vendor = SDL_GetGamepadVendor(gamepad);
    traits.product = SDL_GetGamepadProduct(gamepad);
    traits.type =
        pc::pad_type_of(traits.vendor, traits.product, reported_type(SDL_GetGamepadType(gamepad)));
    traits.trackpads =
        static_cast<uint8_t>(std::clamp(SDL_GetNumGamepadTouchpads(gamepad), 0, 255));
    traits.grips = SDL_GamepadHasButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1) &&
                   SDL_GamepadHasButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1) &&
                   SDL_GamepadHasButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2) &&
                   SDL_GamepadHasButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2);
    traits.gyro = SDL_GamepadHasSensor(gamepad, SDL_SENSOR_GYRO);
    traits.steam_input = SDL_GetGamepadSteamHandle(gamepad) != 0;
}

void PadAccess::note_input(Runtime& runtime, SDL_JoystickID id) {
    auto& state = runtime.pad_state();
    state.used = true;
    state.ahead_of_mouse = true;
    if (id != 0 && open_pad_of(state.pads, id) != nullptr) {
        state.active = id;
        return;
    }
    if (open_pad_of(state.pads, state.active) == nullptr)
        if (const auto* pad = active_pad(runtime); pad != nullptr)
            state.active = pad->id;
}

void PadAccess::take_button(Runtime& runtime, pc::PadButton button, bool down) {
    auto& state = runtime.pad_state();
    const auto index = static_cast<std::size_t>(button);
    if (index >= state.down.size() || state.down[index] == down)
        return;
    state.down[index] = down;
    const uint64_t now = now_ns(runtime);
    if (down)
        press(runtime, button, now);
    else
        release(runtime, button, now);
    // R5 gives FORCE while it is held.
    auto& after = runtime.pad_state();
    after.force =
        action_held(runtime, pc::Action::force) || action_held(runtime, pc::Action::standing_layer);
    if (const bool force = runtime.pad_force_held(); force != after.force_seen) {
        after.force_seen = force;
        runtime.refresh_pointer_modifiers();
    }
}

void PadAccess::sync_sensors(Runtime& runtime, const pc::PadSettings& chosen) {
    if (!runtime.pad_)
        return;
    const bool wanted = chosen.gyro != pc::Gyro::off;
    for (auto& pad : runtime.pad_->pads) {
        if (pad.id == 0 || pad.gamepad == nullptr || !pad.traits.gyro || pad.gyro_on == wanted)
            continue;
        // A refused change is not asked for again every frame.
        if (!SDL_SetGamepadSensorEnabled(pad.gamepad, SDL_SENSOR_GYRO, wanted) && wanted)
            std::cerr << "warning: the gamepad's gyro did not start: " << SDL_GetError() << '\n';
        pad.gyro_on = wanted;
        if (wanted)
            runtime.pad_->gyro_pointer.reset();
    }
}

void PadAccess::forget_held(Runtime& runtime, bool release_engine) {
    auto& state = runtime.pad_state();
    auto* touch = runtime.touch_.get();
    for (std::size_t index = 0; index < state.down.size(); ++index) {
        if (state.latches[index] && touch != nullptr)
            touch->hud.latches.cancel(*state.latches[index]);
        state.latches[index].reset();
        state.holds[index].cancel();
        state.pressed[index] = {};
        state.taps[index] = {};
        state.deferred[index] = false;
        state.down[index] = false;
    }
    // A pad that went away mid-press leaves no button down in the engine:
    // a box is dropped and nothing is clicked.
    if (release_engine && runtime.match_ && (state.left_holders > 0 || state.right_holders > 0)) {
        namespace input = oa::sim::gameplay_input;
        if (state.left_holders > 0)
            runtime.match_drag_.reset();
        runtime.match_->state().game.pointer_state[2] &=
            ~(input::pointer_key_left | input::pointer_key_right);
        runtime.match_hud_held_.reset();
        runtime.selected_ = -1;
    }
    state.left_holders = 0;
    state.right_holders = 0;
    state.box_felt = false;
    state.minimap = false;
    state.jumped = false;
    state.pointer.stop();
    state.drag.stop();
    state.stick_cursor.reset();
    state.flick.reset();
    state.dpad = {};
    state.stick_steps = {};
    state.wheel = {};
    state.panel = {};
    if (touch != nullptr) {
        if (state.ring.kind == PadRingKind::build)
            touch->hud.build_ring.reset();
        else if (release_engine && state.ring.kind == PadRingKind::order && state.ring.by_pad)
            touch->hud.radial.reset();
        touch->hud.sheet_focus = -1;
    }
    state.ring = {};
    state.group_aim.reset();
    state.group_aimed.reset();
    state.sheet_focus = -1;
    state.force = false;
    if (const bool force = runtime.pad_force_held(); force != state.force_seen) {
        state.force_seen = force;
        runtime.refresh_pointer_modifiers();
    }
}

void PadAccess::write_look(Runtime& runtime, const pc::PadSettings& chosen) {
    auto& state = runtime.pad_state();
    auto& touch = runtime.touch_state();
    auto& look = touch.hud.pad;
    const auto context = map_context(runtime);
    const auto now_held = held(runtime);
    const bool on_match = now_held.in_match;
    look.hud = runtime.pad_used() && !runtime.touch_controls_active();
    look.badges = runtime.pad_used() && pc::prompts_shown(chosen.prompts);
    look.glyphs = pc::glyph_style_for(chosen.prompts, traits(runtime).type);
    look.map = context;
    look.force_shown = !context.fallback;
    look.force_active = runtime.pad_force_held();
    look.groups_layer = on_match && now_held.groups;
    look.over_build_button = pointer_on_build_button(runtime);
    // The ring's aim, its dot (the thumb's place on the ring) and its hint.
    look.radial_aim.reset();
    look.build_aim.reset();
    look.ring_by_pad = false;
    const auto ring_point = [&state](hud::Point centre, int outer_radius) {
        const float length = std::hypot(state.ring.offset.x, state.ring.offset.y);
        const float scale = length > 1.0F ? 1.0F / length : 1.0F;
        return hud::Point{
            centre.x + static_cast<int>(std::lround(
                           state.ring.offset.x * scale * static_cast<float>(outer_radius)
                       )),
            centre.y + static_cast<int>(std::lround(
                           state.ring.offset.y * scale * static_cast<float>(outer_radius)
                       ))
        };
    };
    if (state.ring.kind == PadRingKind::order && touch.hud.radial) {
        look.radial_aim = state.ring.aim.slot();
        look.aim_dot = ring_point(touch.hud.radial->centre, touch.hud.radial->outer_radius);
        look.ring_by_pad = state.ring.by_pad;
    } else if (state.ring.kind == PadRingKind::build && touch.hud.build_ring) {
        look.build_aim = state.ring.aim.slot();
        look.aim_dot = ring_point(touch.hud.build_ring->centre, touch.hud.build_ring->outer_radius);
        look.ring_by_pad = state.ring.by_pad;
    }
    // The left pad's ring of the nine groups while the groups layer is held.
    if (on_match && now_held.groups && context.trackpads) {
        auto ring = hud::make_group_ring(ring_viewport(runtime));
        ring.aim = state.group_aimed;
        look.group_ring = ring;
    } else {
        look.group_ring.reset();
    }
    // A timed hold's ring: self-destruct by R5 + X at the pointer, or the
    // standing-orders ring's SELF-DESTRUCT wedge.
    look.hold_progress = 0.0F;
    const uint64_t now_ms = now_ns(runtime) / kNanosecondsPerMillisecond;
    for (std::size_t index = 0; index < state.down.size(); ++index)
        if (state.down[index] && state.pressed[index].action == pc::Action::self_destruct &&
            state.holds[index].down()) {
            look.hold_progress = state.holds[index].progress(now_ms, pc::self_destruct_hold_ms);
            look.hold_point = {
                static_cast<int>(std::lround(state.pointer_x)),
                static_cast<int>(std::lround(state.pointer_y))
            };
        }
    if (state.ring.kind == PadRingKind::build && state.ring.self_destruct_ms &&
        !state.ring.self_destruct_fired && touch.hud.build_ring) {
        const uint64_t since = *state.ring.self_destruct_ms;
        look.hold_progress = std::clamp(
            static_cast<float>(now_ms > since ? now_ms - since : 0) /
                static_cast<float>(pc::self_destruct_hold_ms),
            0.0F,
            1.0F
        );
        if (const auto slot = state.ring.aim.slot(); slot && *slot < hud::build_ring_slot_count) {
            const auto& hit = touch.hud.build_ring->wedges[*slot].hit;
            look.hold_point = {hit.x + hit.width / 2, hit.y + hit.height / 2};
        }
    }
    // The D-pad's mark on SELECT ▾, while it is open.
    if (touch.hud.sheet != hud::Sheet::select_menu)
        state.sheet_focus = -1;
    touch.hud.sheet_focus = state.sheet_focus;
}

bool PadAccess::input_held(const Runtime& runtime) {
    const Runtime::PadState* state = runtime.pad_state_if_made();
    if (state == nullptr)
        return false;
    if (std::any_of(state->down.begin(), state->down.end(), [](bool down) { return down; }))
        return true;
    for (const auto& stick : state->sticks)
        if (pc::stick_deflection(stick) > 0.0F)
            return true;
    return state->pointer.touched() || state->drag.touched() || state->pointer.gliding() ||
           state->drag.gliding() || state->ring.kind != PadRingKind::none;
}

pc::PadButton PadAccess::physical(const Runtime& runtime, pc::PadButton role) {
    return runtime.pad_settings().left_handed ? pc::mirrored(role) : role;
}

pc::Side PadAccess::physical_side(const Runtime& runtime, pc::Side role) {
    if (!runtime.pad_settings().left_handed)
        return role;
    return role == pc::Side::left ? pc::Side::right : pc::Side::left;
}

bool PadAccess::action_held(const Runtime& runtime, pc::Action action) {
    const Runtime::PadState* state = runtime.pad_state_if_made();
    if (state == nullptr)
        return false;
    for (std::size_t index = 0; index < state->down.size(); ++index)
        if (state->down[index] && state->pressed[index].action == action)
            return true;
    return false;
}

uint32_t PadAccess::hold_ms(const Runtime& runtime) {
    return runtime.pad_settings().hold_ms;
}

hud::LatchMode PadAccess::latch_mode(Runtime& runtime) {
    return runtime.engine_settings().touch_latches == settings::TouchLatches::one_action
               ? hud::LatchMode::one_action
               : hud::LatchMode::stay_on;
}

void start_gamepad_subsystem() noexcept {
    if (SDL_WasInit(SDL_INIT_GAMEPAD) == SDL_INIT_GAMEPAD)
        return;
    if (SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        return;
    std::cerr << "warning: gamepads are not available: " << SDL_GetError() << '\n';
}

} // namespace oa::app
