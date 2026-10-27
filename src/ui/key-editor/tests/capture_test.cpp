// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Capturing a chord, settling a clash, and finding by key. The fixture's
// own Ctrl+B on Select all clashes with Select builders. Physical Ctrl is
// stored as Primary on this fixture, and it is drawn as Ctrl.
//
// oa-ui-key-editor-capture-test

#include "oa/data/input_actions/actions.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/key_editor/model.hpp"
#include "oa/ui/key_editor/screen.hpp"
#include "screen_fixture.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace editor = oa::ui::key_editor;
namespace keys = oa::ui::key_bindings;
namespace kit = oa::ui::kit;
namespace acts = oa::data::input_actions;
using key_editor_fixture::editor_fixture;
using key_editor_fixture::fonts;
using key_editor_fixture::frame_for;
using key_editor_fixture::parsed_chord;

/// Prints a line when a check is about to fail, then checks it.
///
/// @param condition the check
/// @param detail what failed, for the line before OA_CHECK
void expect(bool condition, const std::string& detail) {
    if (!condition)
        std::fprintf(stderr, "%s\n", detail.c_str());
    OA_CHECK(condition);
}

/// The position number of a UI Events code. 0 when the table has none.
///
/// @param code the code, such as KeyQ
/// @return the position
uint16_t position(std::string_view code) {
    const std::optional<uint16_t> scan = keys::scancode_of(code);
    expect(scan.has_value(), std::string(code) + " has no position");
    return scan ? *scan : uint16_t{0};
}

/// One key event. Modifiers are the physical bits, never Primary.
///
/// @param code the UI Events code
/// @param key_code the character, or a named key's code
/// @param modifiers the physical modifiers
/// @param time_ms when it happened
/// @param down the key went down
/// @return the event
keys::InputEvent key_event(
    std::string_view code, uint32_t key_code, uint8_t modifiers, uint64_t time_ms, bool down
) {
    keys::InputEvent event;
    event.kind = keys::InputKind::key;
    event.down = down;
    event.scancode = position(code);
    event.key_code = key_code;
    event.modifiers = modifiers;
    event.time_ms = time_ms;
    return event;
}

/// How an action's chords are drawn, for a failing line.
///
/// @param model the editor
/// @param id the action
/// @return the chords, or "none"
std::string chord_line(const editor::Editor& model, std::string_view id) {
    std::string line;
    const editor::Details details = model.details(id);
    for (const editor::ChordDetail& one : details.chords) {
        if (!line.empty())
            line += ", ";
        line += keys::display_text(one.chord, model.keyboard(), model.platform());
    }
    if (line.empty())
        return "none";
    return line;
}

/// True when one drawn chord is this text.
///
/// @param model the editor
/// @param id the action
/// @param text the drawn chord
/// @return true when it is bound
bool shows_chord(const editor::Editor& model, std::string_view id, std::string_view text) {
    const editor::Details details = model.details(id);
    for (const editor::ChordDetail& one : details.chords) {
        if (keys::display_text(one.chord, model.keyboard(), model.platform()) == text)
            return true;
    }
    return false;
}

/// True when a clash names an action.
///
/// @param clashes the clashes
/// @param id the action
/// @return true when it is either side
bool names_clash(const std::vector<keys::Clash>& clashes, std::string_view id) {
    for (const keys::Clash& clash : clashes) {
        if (clash.a == id || clash.b == id)
            return true;
    }
    return false;
}

/// A capture of one action, still waiting, with the registry's letter or place.
///
/// @param id the action
/// @param replacing the chord being changed; none for + ADD KEY
/// @return the capture
editor::Capture begin(std::string id, std::optional<std::size_t> replacing = std::nullopt) {
    editor::Capture capture;
    capture.action = std::move(id);
    capture.replacing = replacing;
    capture.starts = keys::Gesture::press;
    const acts::Action* action = acts::find(capture.action);
    if (action != nullptr && action->by == acts::By::place)
        capture.means = acts::By::place;
    else
        capture.means = acts::By::letter;
    return capture;
}

/// The centre of a control, copied out of the list.
///
/// @param list the display list
/// @param name the automation name
/// @return the centre; none when the control is absent
std::optional<kit::Point> centre_named(const kit::DisplayList& list, std::string_view name) {
    const kit::Control* control = kit::control_of(list, kit::control_named(list, name));
    if (control == nullptr || control->rect.width <= 0 || control->rect.height <= 0)
        return std::nullopt;
    return kit::Point{
        control->rect.x + control->rect.width / 2, control->rect.y + control->rect.height / 2
    };
}

/// A primary click, down then up.
///
/// @param[in,out] model the editor
/// @param[in,out] state the screen
/// @param list the display list the pointer was aimed at
/// @param at the point, already copied out of the list
void click_at(
    editor::Editor& model, editor::ScreenState& state, const kit::DisplayList& list, kit::Point at
) {
    editor::editor_pointer(model, state, list, editor::PointerKind::down, 1, at, 0);
    editor::editor_pointer(model, state, list, editor::PointerKind::up, 1, at, 0);
}

/// Ctrl held, then Q. The chord is drawn Ctrl+Q and the modifier showed as held.
void held_modifier_then_key() {
    editor::Editor model(editor_fixture());
    editor::Capture capture = begin("order.attack");
    const keys::InputEvent held = key_event("ControlLeft", 0, keys::ctrl, 10, true);
    expect(
        editor::capture_key(capture, model, held) == editor::CaptureEvent::changed,
        "Ctrl down did not change the capture"
    );
    expect(capture.state == editor::CaptureState::waiting, "Ctrl down finished the chord");
    expect(!capture.chord.has_value(), "Ctrl down stored a chord");
    expect(capture.held_modifiers == keys::primary, "Ctrl was not stored as Primary");

    const keys::InputEvent pressed =
        key_event("KeyQ", static_cast<uint32_t>('q'), keys::ctrl, 20, true);
    expect(
        editor::capture_key(capture, model, pressed) == editor::CaptureEvent::changed,
        "Q did not finish"
    );
    expect(capture.state == editor::CaptureState::complete, "Ctrl+Q was not free");
    expect(capture.chord.has_value(), "Ctrl+Q stored no chord");
    if (capture.chord) {
        const std::string drawn =
            keys::display_text(*capture.chord, model.keyboard(), model.platform());
        expect(drawn == "Ctrl+Q", "Ctrl+Q was drawn as " + drawn);
        expect(capture.chord->kind == keys::TriggerKind::character, "Ctrl+Q was not a letter");
    }
}

/// A button and a wheel turn each finish a waiting capture.
void button_and_wheel() {
    editor::Editor model(editor_fixture());
    editor::Capture button = begin("order.attack");
    keys::InputEvent click;
    click.kind = keys::InputKind::button;
    click.down = true;
    click.button = 1;
    editor::capture_button(button, model, click);
    expect(button.chord.has_value(), "a button stored no chord");
    if (button.chord) {
        expect(button.chord->kind == keys::TriggerKind::mouse, "a button was not a mouse chord");
        expect(button.chord->button == 1, "button 1 was stored as another button");
    }
    expect(button.state != editor::CaptureState::waiting, "a button left the capture waiting");

    editor::Capture wheel = begin("order.attack");
    keys::InputEvent turn;
    turn.kind = keys::InputKind::wheel;
    turn.wheel_y = 1;
    editor::capture_wheel(wheel, model, turn);
    expect(wheel.chord.has_value(), "a wheel turn stored no chord");
    if (wheel.chord) {
        expect(wheel.chord->kind == keys::TriggerKind::wheel, "a wheel turn was not a wheel chord");
        expect(wheel.chord->wheel == 1, "wheel up was not notch 1");
    }
    expect(wheel.state != editor::CaptureState::waiting, "a wheel turn left the capture waiting");
}

/// Letter or place follows the registry, and the prompt can switch either, and the gesture.
void means_and_gestures() {
    editor::Editor model(editor_fixture());
    editor::ScreenState state;
    state.selected = "order.attack";
    const kit::DisplayList unused;
    editor::editor_command(model, state, unused, editor::EditorCommand::add_key);
    expect(state.prompt == editor::Prompt::capture, "+ ADD KEY did not open the capture");
    expect(!state.capture.replacing.has_value(), "+ ADD KEY named a chord to replace");
    expect(state.capture.means == acts::By::letter, "Attack did not start as a letter");

    editor::capture_key(
        state.capture, model, key_event("KeyQ", static_cast<uint32_t>('q'), 0, 30, true)
    );
    expect(state.capture.chord.has_value(), "Q stored no chord");
    if (state.capture.chord)
        expect(state.capture.chord->kind == keys::TriggerKind::character, "Q was not a letter");

    const acts::Action* box = acts::find("select.box-combat");
    expect(box != nullptr && box->by == acts::By::place, "Box combat is not bound by place");
    editor::Capture placed = begin("select.box-combat");
    expect(placed.means == acts::By::place, "Box combat did not start as a place");
    editor::capture_key(placed, model, key_event("KeyW", static_cast<uint32_t>('w'), 0, 40, true));
    expect(placed.chord.has_value(), "KeyW stored no chord");
    if (placed.chord)
        expect(placed.chord->kind == keys::TriggerKind::place, "KeyW was not a place");

    editor::set_means(state.capture, model, acts::By::place);
    expect(state.capture.means == acts::By::place, "Means did not switch to the place");
    if (state.capture.chord)
        expect(state.capture.chord->kind == keys::TriggerKind::place, "the chord stayed a letter");
    editor::set_means(state.capture, model, acts::By::letter);
    expect(state.capture.means == acts::By::letter, "Means did not switch back to the letter");
    if (state.capture.chord)
        expect(
            state.capture.chord->kind == keys::TriggerKind::character, "the chord stayed a place"
        );

    editor::set_gesture(state.capture, model, keys::Gesture::double_press);
    expect(state.capture.starts == keys::Gesture::double_press, "a double press was not kept");
    if (state.capture.chord)
        expect(
            state.capture.chord->gesture == keys::Gesture::double_press,
            "the chord is not a double press"
        );
    expect(
        state.capture.state == editor::CaptureState::complete, "a double press was no longer free"
    );
    editor::set_gesture(state.capture, model, keys::Gesture::hold);
    expect(state.capture.starts == keys::Gesture::hold, "a hold was not kept");
    if (state.capture.chord)
        expect(state.capture.chord->gesture == keys::Gesture::hold, "the chord is not a hold");
    expect(state.capture.state == editor::CaptureState::complete, "a hold was no longer free");
}

/// A lone Shift finishes Queue and only shows as held for Attack.
void lone_shift() {
    editor::Editor model(editor_fixture());
    editor::Capture queue = begin("order.queue-modifier");
    editor::capture_key(queue, model, key_event("ShiftLeft", 0, keys::shift, 50, true));
    expect(queue.state != editor::CaptureState::waiting, "Shift did not finish Queue");
    expect(queue.chord.has_value(), "Shift stored no chord for Queue");
    if (queue.chord)
        expect(
            queue.chord->kind == keys::TriggerKind::modifier_alone,
            "Queue's Shift was not a modifier"
        );

    editor::Capture attack = begin("order.attack");
    editor::capture_key(attack, model, key_event("ShiftLeft", 0, keys::shift, 60, true));
    expect(attack.state == editor::CaptureState::waiting, "Shift finished Attack");
    expect(!attack.chord.has_value(), "Shift stored a chord for Attack");
    expect(attack.held_modifiers == keys::shift, "Shift was not held for Attack");
}

/// Escape tapped cancels. 900 ms does not bind it. 1,000 ms does.
void escape_timer() {
    editor::Editor model(editor_fixture());
    editor::Capture tapped = begin("order.attack");
    expect(
        editor::capture_key(tapped, model, key_event("Escape", 0x1B, 0, 100, true)) ==
            editor::CaptureEvent::changed,
        "Escape down did not start"
    );
    expect(tapped.state == editor::CaptureState::waiting, "Escape down finished at once");
    expect(
        editor::capture_key(tapped, model, key_event("Escape", 0x1B, 0, 200, false)) ==
            editor::CaptureEvent::cancelled,
        "Escape up did not cancel"
    );

    editor::ScreenState state;
    state.selected = "order.attack";
    state.prompt = editor::Prompt::capture;
    state.capture = begin("order.attack");
    const kit::DisplayList unused;
    editor::editor_capture_input(model, state, key_event("Escape", 0x1B, 0, 100, true));
    expect(state.prompt == editor::Prompt::capture, "Escape down closed the prompt");
    editor::editor_capture_input(model, state, key_event("Escape", 0x1B, 0, 200, false));
    expect(state.prompt == editor::Prompt::none, "Escape up left the prompt open");

    editor::Capture held = begin("order.attack");
    editor::capture_key(held, model, key_event("Escape", 0x1B, 0, 100, true));
    editor::capture_tick(held, model, 1000);
    expect(held.state == editor::CaptureState::waiting, "Escape bound itself at 900 ms");
    expect(!held.chord.has_value(), "Escape stored a chord at 900 ms");
    editor::capture_tick(held, model, 1100);
    expect(held.state != editor::CaptureState::waiting, "Escape did not bind at 1,000 ms");
    expect(held.chord.has_value(), "Escape stored no chord at 1,000 ms");
    if (held.chord) {
        expect(held.chord->kind == keys::TriggerKind::named, "held Escape was not a named key");
        expect(
            held.chord->named == static_cast<uint8_t>(keys::NamedKey::escape),
            "held Escape was not Escape"
        );
    }
}

/// Escape for Send is refused. Escape for Attack clashes with Back. Primary+, clashes with Settings.
void refused_and_settings() {
    editor::Editor model(editor_fixture());
    editor::Capture send = begin("chat.send");
    editor::capture_key(send, model, key_event("Escape", 0x1B, 0, 1000, true));
    editor::capture_tick(send, model, 2000);
    expect(send.state == editor::CaptureState::refused, "Escape for Send was not refused");
    const editor::CaptureStatus refused = editor::capture_status(model, send);
    expect(refused.refused, "Escape for Send did not count as fixed");
    expect(!refused.ok_enabled, "OK was enabled for a refused Escape");
    expect(
        refused.line.find("Choose another key") != std::string::npos,
        "the refusal did not say to choose another key: " + refused.line
    );
    expect(
        refused.line.find("Open Annihilation") != std::string::npos,
        "the refusal did not name Open Annihilation: " + refused.line
    );

    editor::Capture attack = begin("order.attack");
    editor::capture_key(attack, model, key_event("Escape", 0x1B, 0, 1000, true));
    editor::capture_tick(attack, model, 2000);
    expect(attack.state == editor::CaptureState::clash, "Escape for Attack was not a clash");
    const editor::CaptureStatus back = editor::capture_status(model, attack);
    expect(!back.refused, "Escape for Attack was refused as fixed");
    expect(back.ok_enabled, "OK was disabled for the clash with Back");
    expect(
        back.line.find("Back") != std::string::npos, "the clash did not name Back: " + back.line
    );
    expect(
        names_clash(model.clashes_for("order.attack", parsed_chord("Escape")), "game.back"),
        "Escape did not clash with game.back"
    );

    editor::Capture comma = begin("order.attack");
    editor::capture_key(
        comma, model, key_event("Comma", static_cast<uint32_t>(','), keys::ctrl, 70, true)
    );
    expect(comma.state == editor::CaptureState::clash, "Ctrl+, was not a clash");
    const editor::CaptureStatus settings = editor::capture_status(model, comma);
    expect(!settings.refused, "Ctrl+, was refused as fixed");
    expect(
        settings.line.find("Choose another key") == std::string::npos,
        "Ctrl+, was told to choose another key: " + settings.line
    );
    expect(
        settings.line.find("Settings") != std::string::npos,
        "Ctrl+, did not name Settings: " + settings.line
    );
    if (comma.chord) {
        expect(
            names_clash(model.clashes_for("order.attack", *comma.chord), "ui.settings"),
            "Ctrl+, did not clash with ui.settings"
        );
    }

    editor::EditorInputs inputs = editor_fixture();
    inputs.platform.primary_is_meta = true;
    editor::Editor mac(std::move(inputs));
    editor::Capture command = begin("order.attack");
    editor::capture_key(
        command, mac, key_event("Comma", static_cast<uint32_t>(','), keys::meta, 80, true)
    );
    expect(command.state == editor::CaptureState::clash, "Cmd+, was not a clash");
    const editor::CaptureStatus command_line = editor::capture_status(mac, command);
    expect(!command_line.refused, "Cmd+, was refused as fixed");
    expect(
        command_line.line.find("Settings") != std::string::npos,
        "Cmd+, did not name Settings: " + command_line.line
    );
}

/// Tab in single player and Tab in multiplayer are not a clash.
void tab_is_not_a_clash() {
    const editor::Editor model(editor_fixture());
    const keys::Chord tab = parsed_chord("Tab");
    const std::vector<keys::Clash> single = model.clashes_for("select.next-primary", tab);
    const std::vector<keys::Clash> allies = model.clashes_for("game.team-menu", tab);
    expect(
        !names_clash(single, "game.team-menu"), "Tab in single player clashed with the allies menu"
    );
    expect(
        !names_clash(allies, "select.next-primary"), "Tab in multiplayer clashed with Next primary"
    );
}

/// REPLACE, SWAP and CANCEL, and the row REPLACE chooses.
void replace_swap_cancel() {
    editor::Editor cancelled(editor_fixture());
    expect(cancelled.reset_action("select.all"), "Select all did not return to Ctrl+A");
    editor::Capture capture = begin("select.all", 0);
    editor::capture_key(
        capture, cancelled, key_event("KeyB", static_cast<uint32_t>('b'), keys::ctrl, 90, true)
    );
    expect(capture.state == editor::CaptureState::clash, "Ctrl+B on Select all was not a clash");
    expect(
        shows_chord(cancelled, "select.all", "Ctrl+A"),
        "CANCEL changed Select all to " + chord_line(cancelled, "select.all")
    );
    expect(
        shows_chord(cancelled, "select.builders", "Ctrl+B"),
        "CANCEL changed Select builders to " + chord_line(cancelled, "select.builders")
    );

    editor::Editor replaced(editor_fixture());
    expect(
        replaced.reset_action("select.all"), "Select all did not return to Ctrl+A before Replace"
    );
    editor::Capture replacing = begin("select.all", 0);
    editor::capture_key(
        replacing, replaced, key_event("KeyB", static_cast<uint32_t>('b'), keys::ctrl, 90, true)
    );
    const editor::ApplyResult replaced_result =
        editor::apply_capture(replaced, replacing, editor::ClashChoice::replace);
    expect(replaced_result.applied, "REPLACE wrote nothing");
    expect(replaced_result.select == "select.builders", "REPLACE chose " + replaced_result.select);
    expect(
        shows_chord(replaced, "select.all", "Ctrl+B"),
        "REPLACE left Select all as " + chord_line(replaced, "select.all")
    );
    expect(
        replaced.details("select.builders").chords.empty(),
        "REPLACE left Select builders as " + chord_line(replaced, "select.builders")
    );

    editor::Editor swapped(editor_fixture());
    expect(swapped.reset_action("select.all"), "Select all did not return to Ctrl+A before Swap");
    editor::Capture swapping = begin("select.all", 0);
    editor::capture_key(
        swapping, swapped, key_event("KeyB", static_cast<uint32_t>('b'), keys::ctrl, 90, true)
    );
    const editor::ApplyResult swapped_result =
        editor::apply_capture(swapped, swapping, editor::ClashChoice::swap);
    expect(swapped_result.applied, "SWAP wrote nothing");
    expect(swapped_result.select == "select.all", "SWAP chose " + swapped_result.select);
    expect(
        shows_chord(swapped, "select.all", "Ctrl+B"),
        "SWAP left Select all as " + chord_line(swapped, "select.all")
    );
    expect(
        shows_chord(swapped, "select.builders", "Ctrl+A"),
        "SWAP left Select builders as " + chord_line(swapped, "select.builders")
    );
}

/// True when two rectangles share a row.
///
/// @param first one rectangle
/// @param second the other
/// @return true when their rows meet
bool shares_row(const kit::Rect& first, const kit::Rect& second) {
    return first.height > 0 && second.height > 0 && first.y < second.y + second.height &&
           second.y < first.y + first.height;
}

/// The first item of a role whose text is exactly `word`, after the dimming veil.
///
/// @param list the display list
/// @param role the role
/// @param word the words
/// @return the item; none when the prompt has no such item
const kit::Item* prompt_item(const kit::DisplayList& list, kit::Role role, std::string_view word) {
    bool prompt = false;
    for (const kit::Item& item : list.items) {
        if (item.role == kit::Role::blend)
            prompt = true;
        if (prompt && item.role == role && item.text == word)
            return &item;
    }
    return nullptr;
}

/// How many chord chips in the prompt show exactly `word`.
///
/// @param list the display list
/// @param word the chip's words
/// @return the count
int prompt_chips(const kit::DisplayList& list, std::string_view word) {
    bool prompt = false;
    int count = 0;
    for (const kit::Item& item : list.items) {
        if (item.role == kit::Role::blend)
            prompt = true;
        if (prompt && item.role == kit::Role::chord_chip && item.text == word)
            ++count;
    }
    return count;
}

/// The capture prompt is a content-sized box: a wide well, labels on the
/// strip's row, and the escape hint on the footer's row.
void capture_prompt_shape() {
    const editor::Editor model(editor_fixture());
    const kit::Frame frames[] = {
        frame_for(640, 480, 100), frame_for(1280, 720, 100), frame_for(1920, 1080, 200)
    };
    for (const kit::Frame& frame : frames) {
        const kit::Point window = editor::window_size(frame);
        editor::ScreenState state;
        state.prompt = editor::Prompt::capture;
        state.selected = "order.attack";
        state.capture = begin("order.attack");
        state.capture.chord = parsed_chord("Ctrl+Q");
        state.capture.state = editor::CaptureState::complete;
        const kit::DisplayList list = editor::editor_layout(model, state, frame, fonts());
        bool veiled = false;
        bool panel = false;
        bool full_fill = false;
        for (const kit::Item& item : list.items) {
            if (item.role == kit::Role::blend) {
                veiled = item.rect.width == window.x && item.rect.height == window.y &&
                         item.opacity == kit::menu_backdrop_opacity;
            }
            if (!veiled || item.role != kit::Role::fill)
                continue;
            if (item.rect.width >= window.x && item.rect.height >= window.y)
                full_fill = true;
            if (item.rect.width < window.x && item.rect.height < window.y && item.rect.x > 0 &&
                item.rect.y > 0 && item.rect.width >= 400)
                panel = true;
        }
        expect(veiled, "the editor is not dimmed behind the capture prompt");
        expect(panel, "the capture prompt is not a centred box");
        expect(!full_fill, "the capture prompt fills the window");
        const kit::Control* well =
            kit::control_of(list, kit::control_named(list, editor::name_capture_well));
        expect(well != nullptr && well->rect.height == 76, "the well is not 76 points high");
        expect(
            well != nullptr && well->rect.width > well->rect.height,
            "the well does not span the prompt"
        );
        expect(
            prompt_item(list, kit::Role::text, "Same place on every keyboard") == nullptr,
            "More options are open before they are asked for"
        );
        const kit::Control* said =
            kit::control_of(list, kit::control_named(list, editor::name_capture_status));
        expect(
            said != nullptr && said->text == "Ctrl+Q is free while playing.",
            "the capture does not say Ctrl+Q is free while playing"
        );
        const kit::Control* use =
            kit::control_of(list, kit::control_named(list, editor::name_capture_ok));
        expect(
            use != nullptr && use->text == "Use Ctrl+Q for Attack",
            "the capture's button is not Use Ctrl+Q for Attack"
        );
        state.capture_more = true;
        const kit::DisplayList more = editor::editor_layout(model, state, frame, fonts());
        const kit::Item* means = prompt_item(more, kit::Role::text, "Same place on every keyboard");
        const kit::Control* place =
            kit::control_of(more, kit::control_named(more, editor::name_capture_means));
        expect(
            means != nullptr && place != nullptr && shares_row(means->rect, place->rect),
            "Same place on every keyboard is not on its switch's row"
        );
        const kit::Item* starts = prompt_item(more, kit::Role::text, "How it acts");
        const kit::Control* press =
            kit::control_of(more, kit::control_named(more, editor::name_capture_press));
        expect(
            starts != nullptr && press != nullptr && shares_row(starts->rect, press->rect),
            "How it acts is not on the strip's row"
        );
        const kit::Control* cancel =
            kit::control_of(list, kit::control_named(list, editor::name_capture_cancel));
        bool hint_on_footer = false;
        bool prompt = false;
        for (const kit::Item& item : list.items) {
            if (item.role == kit::Role::blend)
                prompt = true;
            if (prompt && item.role == kit::Role::text &&
                item.text.find("Escape cancels") != std::string::npos && cancel != nullptr &&
                shares_row(item.rect, cancel->rect) && item.rect.x < cancel->rect.x)
                hint_on_footer = true;
        }
        expect(hint_on_footer, "the escape hint is not on the footer's row");
    }
}

/// The clash question lists each outcome's result on both actions: Move it
/// leaves the other with no key, Swap them gives it the chord given up.
void clash_row_words() {
    editor::Editor model(editor_fixture());
    expect(model.reset_action("select.all"), "Select all did not return to Ctrl+A for the row");
    editor::Capture capture = begin("select.all", 0);
    editor::capture_key(
        capture, model, key_event("KeyB", static_cast<uint32_t>('b'), keys::ctrl, 90, true)
    );
    expect(capture.state == editor::CaptureState::clash, "the row's Ctrl+B was not a clash");
    editor::ScreenState state;
    state.prompt = editor::Prompt::clash;
    state.capture = capture;
    state.selected = "select.all";
    const editor::CaptureStatus status = editor::capture_status(model, capture);
    expect(status.clash.has_value(), "the row's clash was not named");
    if (status.clash)
        state.pending_clash = *status.clash;
    const kit::Frame frame = frame_for(1280, 720, 100);
    const kit::DisplayList list = editor::editor_layout(model, state, frame, fonts());
    expect(
        prompt_item(list, kit::Role::text, "Ctrl+B is already in use") != nullptr,
        "the question is not Ctrl+B is already in use"
    );
    const kit::Control* move =
        kit::control_of(list, kit::control_named(list, editor::name_clash_replace));
    const kit::Control* swap =
        kit::control_of(list, kit::control_named(list, editor::name_clash_swap));
    expect(move != nullptr && move->checked, "Move it is not the marked outcome");
    expect(swap != nullptr && !swap->checked, "Swap them is missing or marked");
    expect(
        move != nullptr && move->text.find("Select builders: no key") != std::string::npos,
        "Move it does not leave Select builders with no key"
    );
    expect(
        swap != nullptr && swap->text.find("Select builders: Ctrl+A") != std::string::npos,
        "Swap them does not give Select builders Ctrl+A"
    );
    expect(prompt_chips(list, "Ctrl+B") == 2, "the outcomes do not show Ctrl+B twice");
    expect(prompt_chips(list, "Ctrl+A") == 1, "Swap them does not show Ctrl+A");
    const kit::Control* cancel =
        kit::control_of(list, kit::control_named(list, editor::name_clash_cancel));
    const kit::Control* confirm =
        kit::control_of(list, kit::control_named(list, editor::name_clash_confirm));
    const kit::Control* another =
        kit::control_of(list, kit::control_named(list, editor::name_clash_another));
    expect(
        confirm != nullptr && confirm->text == "Move to Select all",
        "the confirm button is not Move to Select all"
    );
    expect(
        another != nullptr && cancel != nullptr && confirm != nullptr &&
            another->rect.x < cancel->rect.x && cancel->rect.x < confirm->rect.x,
        "the footer is not Choose another key, Cancel, Move to Select all"
    );

    editor::Editor adding(editor_fixture());
    editor::Capture added = begin("order.attack");
    editor::capture_key(
        added, adding, key_event("KeyB", static_cast<uint32_t>('b'), keys::ctrl, 90, true)
    );
    editor::ScreenState add_state;
    add_state.prompt = editor::Prompt::clash;
    add_state.capture = added;
    add_state.selected = "order.attack";
    if (const editor::CaptureStatus add_status = editor::capture_status(adding, added);
        add_status.clash)
        add_state.pending_clash = *add_status.clash;
    const kit::DisplayList add_list = editor::editor_layout(adding, add_state, frame, fonts());
    expect(
        kit::control_of(add_list, kit::control_named(add_list, editor::name_clash_swap)) == nullptr,
        "+ Add a key offers Swap them"
    );
    expect(
        prompt_item(add_list, kit::Role::text, "No key") != nullptr,
        "+ Add a key does not leave the other action with no key"
    );
}

/// The clash question changes nothing until its confirm button: choosing an
/// outcome only marks it, Cancel and Choose another key leave every chord.
void outcomes_wait_for_confirm() {
    const kit::Frame frame = frame_for(1280, 720, 100);
    editor::Editor model(editor_fixture());
    expect(model.reset_action("select.all"), "Select all did not return to Ctrl+A");
    const std::size_t opened = model.unsaved();
    editor::ScreenState state;
    state.selected = "select.all";
    state.prompt = editor::Prompt::capture;
    state.capture = begin("select.all", 0);
    editor::capture_key(
        state.capture, model, key_event("KeyB", static_cast<uint32_t>('b'), keys::ctrl, 90, true)
    );
    kit::DisplayList list = editor::editor_layout(model, state, frame, fonts());
    if (const std::optional<kit::Point> ok = centre_named(list, editor::name_capture_ok))
        click_at(model, state, list, *ok);
    expect(state.prompt == editor::Prompt::clash, "Choose what happens did not ask");
    expect(
        shows_chord(model, "select.all", "Ctrl+A") &&
            shows_chord(model, "select.builders", "Ctrl+B"),
        "opening the question changed a chord"
    );
    list = editor::editor_layout(model, state, frame, fonts());
    if (const std::optional<kit::Point> swap = centre_named(list, editor::name_clash_swap))
        click_at(model, state, list, *swap);
    expect(state.outcome == 1, "Swap them was not marked");
    expect(
        shows_chord(model, "select.all", "Ctrl+A") &&
            shows_chord(model, "select.builders", "Ctrl+B"),
        "marking Swap them changed a chord"
    );
    list = editor::editor_layout(model, state, frame, fonts());
    if (const std::optional<kit::Point> another = centre_named(list, editor::name_clash_another))
        click_at(model, state, list, *another);
    expect(state.prompt == editor::Prompt::capture, "Choose another key did not listen again");
    expect(!state.capture.chord.has_value(), "Choose another key kept the chord");
    expect(model.unsaved() == opened, "Choose another key changed the draft");

    editor::capture_key(
        state.capture, model, key_event("KeyB", static_cast<uint32_t>('b'), keys::ctrl, 95, true)
    );
    list = editor::editor_layout(model, state, frame, fonts());
    if (const std::optional<kit::Point> ok = centre_named(list, editor::name_capture_ok))
        click_at(model, state, list, *ok);
    list = editor::editor_layout(model, state, frame, fonts());
    if (const std::optional<kit::Point> cancel = centre_named(list, editor::name_clash_cancel))
        click_at(model, state, list, *cancel);
    expect(state.prompt == editor::Prompt::none, "Cancel did not close the question");
    expect(model.unsaved() == opened, "Cancel changed the draft");

    state.prompt = editor::Prompt::capture;
    state.capture = begin("select.all", 0);
    editor::capture_key(
        state.capture, model, key_event("KeyB", static_cast<uint32_t>('b'), keys::ctrl, 99, true)
    );
    list = editor::editor_layout(model, state, frame, fonts());
    if (const std::optional<kit::Point> ok = centre_named(list, editor::name_capture_ok))
        click_at(model, state, list, *ok);
    list = editor::editor_layout(model, state, frame, fonts());
    if (const std::optional<kit::Point> swap = centre_named(list, editor::name_clash_swap))
        click_at(model, state, list, *swap);
    list = editor::editor_layout(model, state, frame, fonts());
    if (const std::optional<kit::Point> confirm = centre_named(list, editor::name_clash_confirm))
        click_at(model, state, list, *confirm);
    expect(state.prompt == editor::Prompt::none, "Swap them did not close the question");
    expect(
        shows_chord(model, "select.all", "Ctrl+B") &&
            shows_chord(model, "select.builders", "Ctrl+A"),
        "Swap them did not trade the chords"
    );
    expect(model.can_undo(), "Swap them left nothing to undo");
    expect(model.undo(), "Undo did nothing");
    expect(
        shows_chord(model, "select.all", "Ctrl+A") &&
            shows_chord(model, "select.builders", "Ctrl+B"),
        "Undo did not put both chords back in one step"
    );
}

/// + ADD KEY has no chord to give away, so SWAP writes nothing and the prompt has no SWAP.
void no_swap_for_add() {
    editor::Editor model(editor_fixture());
    editor::Capture capture = begin("order.attack");
    editor::capture_key(
        capture, model, key_event("KeyB", static_cast<uint32_t>('b'), keys::ctrl, 90, true)
    );
    expect(capture.state == editor::CaptureState::clash, "adding Ctrl+B was not a clash");
    const std::string before = chord_line(model, "order.attack");
    const editor::ApplyResult result =
        editor::apply_capture(model, capture, editor::ClashChoice::swap);
    expect(!result.applied, "SWAP wrote a chord for + ADD KEY");
    expect(chord_line(model, "order.attack") == before, "SWAP changed Attack");

    editor::ScreenState state;
    state.prompt = editor::Prompt::clash;
    state.capture = capture;
    state.selected = "order.attack";
    if (const editor::CaptureStatus status = editor::capture_status(model, capture); status.clash)
        state.pending_clash = *status.clash;
    const kit::Frame frame = frame_for(1280, 720, 100);
    const kit::DisplayList added = editor::editor_layout(model, state, frame, fonts());
    expect(
        kit::control_of(added, kit::control_named(added, editor::name_clash_swap)) == nullptr,
        "+ ADD KEY showed SWAP"
    );
    expect(
        kit::control_of(added, kit::control_named(added, editor::name_clash_replace)) != nullptr,
        "+ ADD KEY hid REPLACE"
    );

    state.capture.replacing = 0;
    const kit::DisplayList changing = editor::editor_layout(model, state, frame, fonts());
    expect(
        kit::control_of(changing, kit::control_named(changing, editor::name_clash_swap)) != nullptr,
        "changing a chord hid SWAP"
    );
}

/// The card's three choices, from the model and from a click.
void card_choices() {
    editor::Editor here(editor_fixture());
    const std::optional<keys::Clash> here_clash = here.draft_clash("select.all");
    expect(here_clash.has_value(), "Select all had no draft clash");
    if (here_clash) {
        expect(here_clash->a == "select.all", "the card's here side was " + here_clash->a);
        expect(
            editor::settle(here, *here_clash, editor::Settle::keep_here),
            "KEEP IT HERE wrote nothing"
        );
    }
    expect(here.can_save(), "KEEP IT HERE left SAVE disabled");
    expect(
        here.details("select.builders").chords.empty(),
        "KEEP IT HERE left Select builders as " + chord_line(here, "select.builders")
    );
    expect(
        shows_chord(here, "select.all", "Ctrl+B"),
        "KEEP IT HERE left Select all as " + chord_line(here, "select.all")
    );

    editor::Editor there(editor_fixture());
    const std::optional<keys::Clash> there_clash = there.draft_clash("select.all");
    expect(there_clash.has_value(), "Select all had no draft clash to give away");
    if (there_clash)
        expect(
            editor::settle(there, *there_clash, editor::Settle::keep_there),
            "KEEP IT FOR wrote nothing"
        );
    expect(there.can_save(), "KEEP IT FOR left SAVE disabled");
    expect(
        shows_chord(there, "select.all", "Ctrl+A"),
        "KEEP IT FOR left Select all as " + chord_line(there, "select.all")
    );
    expect(
        shows_chord(there, "select.builders", "Ctrl+B"),
        "KEEP IT FOR left Select builders as " + chord_line(there, "select.builders")
    );

    // In the details a draft clash shows Settle…, which opens the one clash
    // question with Keep it for each side as its outcomes.
    const kit::Frame frame = frame_for(1280, 720, 100);
    editor::Editor choosing(editor_fixture());
    editor::ScreenState state;
    state.selected = "select.all";
    state.filter = editor::Filter::clashes;
    state.category = "all";
    kit::DisplayList list = editor::editor_layout(choosing, state, frame, fonts());
    const std::optional<kit::Point> settle_at = centre_named(list, editor::name_card_settle);
    expect(settle_at.has_value(), "Settle… is not shown");
    if (settle_at)
        click_at(choosing, state, list, *settle_at);
    expect(state.prompt == editor::Prompt::clash && state.settling, "Settle… did not ask");
    list = editor::editor_layout(choosing, state, frame, fonts());
    const kit::Control* keep_here =
        kit::control_of(list, kit::control_named(list, editor::name_card_here));
    const kit::Control* keep_there =
        kit::control_of(list, kit::control_named(list, editor::name_card_there));
    expect(keep_here != nullptr && keep_here->checked, "Keep it for Select all is not marked");
    expect(keep_there != nullptr, "Keep it for Select builders is not shown");
    expect(
        keep_here != nullptr &&
            keep_here->text.find("Select builders: no key") != std::string::npos,
        "Keep it for Select all does not leave Select builders with no key"
    );
    if (const std::optional<kit::Point> there_at = centre_named(list, editor::name_card_there))
        click_at(choosing, state, list, *there_at);
    expect(state.outcome == 1, "Keep it for Select builders was not marked");
    expect(
        shows_chord(choosing, "select.all", "Ctrl+B"),
        "marking an outcome changed Select all to " + chord_line(choosing, "select.all")
    );
    list = editor::editor_layout(choosing, state, frame, fonts());
    if (const std::optional<kit::Point> confirm = centre_named(list, editor::name_clash_confirm))
        click_at(choosing, state, list, *confirm);
    expect(state.prompt == editor::Prompt::none, "the confirm button left the question open");
    expect(choosing.can_save(), "Keep it for Select builders left Save changes disabled");
    expect(
        shows_chord(choosing, "select.all", "Ctrl+A"),
        "Keep it for Select builders left Select all as " + chord_line(choosing, "select.all")
    );

    editor::Editor another(editor_fixture());
    editor::ScreenState again;
    again.selected = "select.all";
    again.filter = editor::Filter::clashes;
    again.category = "all";
    list = editor::editor_layout(another, again, frame, fonts());
    if (const std::optional<kit::Point> at = centre_named(list, editor::name_card_settle))
        click_at(another, again, list, *at);
    list = editor::editor_layout(another, again, frame, fonts());
    if (const std::optional<kit::Point> at = centre_named(list, editor::name_clash_another))
        click_at(another, again, list, *at);
    expect(again.prompt == editor::Prompt::capture, "Choose another key did not open the capture");
    expect(again.capture.replacing.has_value(), "Choose another key did not name the chord");
    expect(
        again.capture.action == "select.all", "Choose another key captured " + again.capture.action
    );
}

/// Find by key listens for a chord apart from the search, lists every
/// action on it and changes nothing; Open › chooses the action. Typing in
/// the search only searches commands.
void find_by_key() {
    const kit::Frame frame = frame_for(1280, 720, 100);
    editor::Editor model(editor_fixture());
    editor::ScreenState state;
    kit::DisplayList list = editor::editor_layout(model, state, frame, fonts());
    if (const std::optional<kit::Point> at = centre_named(list, editor::name_find))
        click_at(model, state, list, *at);
    expect(state.prompt == editor::Prompt::find, "Find by key did not open");
    editor::editor_capture_input(
        model, state, key_event("KeyB", static_cast<uint32_t>('b'), keys::ctrl, 100, true)
    );
    expect(state.finding.chord.has_value(), "Find by key heard no chord");
    if (state.finding.chord) {
        const std::vector<editor::ChordUse> uses =
            editor::find_by_chord(model, *state.finding.chord);
        bool all = false;
        bool builders = false;
        for (const editor::ChordUse& use : uses) {
            if (use.id == "select.all")
                all = true;
            if (use.id == "select.builders")
                builders = true;
        }
        expect(all, "Ctrl+B did not list Select all");
        expect(builders, "Ctrl+B did not list Select builders");
    }
    expect(model.unsaved() == 0, "Find by key changed the draft");
    list = editor::editor_layout(model, state, frame, fonts());
    const kit::Control* line =
        kit::control_of(list, kit::control_named(list, editor::name_find_prompt));
    expect(
        line != nullptr && line->text.find("Nothing is changed.") != std::string::npos,
        "Find by key does not say nothing is changed"
    );
    const std::string first_row = std::string(editor::name_find_row_prefix) + "1";
    const kit::Control* row = kit::control_of(list, kit::control_named(list, first_row));
    expect(row != nullptr, "Find by key lists no row");
    if (const std::optional<kit::Point> at = centre_named(list, first_row))
        click_at(model, state, list, *at);
    expect(state.prompt == editor::Prompt::none, "Open › left Find by key open");
    expect(
        state.selected == "select.all" || state.selected == "select.builders",
        "Open › chose " + state.selected
    );
    expect(model.unsaved() == 0, "Open › changed the draft");

    editor::ScreenState typing;
    const kit::DisplayList unused;
    editor::editor_command(model, typing, unused, editor::EditorCommand::find);
    editor::editor_text(model, typing, unused, "b");
    expect(typing.search.text == "b", "a plain B typed \"" + typing.search.text + "\"");
    expect(typing.prompt == editor::Prompt::none, "typing opened a prompt");
}

/// A fifth chord is refused, and the fixed Escape cannot be dropped.
void limits() {
    editor::Editor model(editor_fixture());
    expect(!model.set_chords("chat.cancel", {}), "the fixed Escape was dropped");
    std::vector<keys::Chord> five;
    for (uint32_t index = 0; index < 5; ++index) {
        keys::Chord chord;
        chord.kind = keys::TriggerKind::character;
        chord.character = static_cast<char32_t>(U'A' + index);
        five.push_back(chord);
    }
    expect(!model.set_chords("order.attack", std::move(five)), "a fifth chord was accepted");
}

/// A click on OK or CANCEL is not a chord. A click or a wheel turn in the well is.
void well_not_the_buttons() {
    const kit::Frame frame = frame_for(1280, 720, 100);
    const kit::DisplayList unused;

    editor::Editor buttons(editor_fixture());
    editor::ScreenState waiting;
    waiting.selected = "order.attack";
    editor::editor_command(buttons, waiting, unused, editor::EditorCommand::add_key);
    const kit::DisplayList prompt = editor::editor_layout(buttons, waiting, frame, fonts());
    const std::optional<kit::Point> ok = centre_named(prompt, editor::name_capture_ok);
    const std::optional<kit::Point> cancel = centre_named(prompt, editor::name_capture_cancel);
    const std::optional<kit::Point> well = centre_named(prompt, editor::name_capture_well);
    expect(ok.has_value(), "the capture has no OK");
    expect(cancel.has_value(), "the capture has no CANCEL");
    expect(well.has_value(), "the capture has no well");
    if (ok)
        click_at(buttons, waiting, prompt, *ok);
    expect(!waiting.capture.chord.has_value(), "a click on OK was taken as a chord");
    expect(waiting.prompt == editor::Prompt::capture, "a click on OK closed a waiting capture");
    if (cancel)
        click_at(buttons, waiting, prompt, *cancel);
    expect(waiting.prompt == editor::Prompt::none, "CANCEL did not close the capture");
    bool mouse = false;
    for (const editor::ChordDetail& one : buttons.details("order.attack").chords) {
        if (one.chord.kind == keys::TriggerKind::mouse)
            mouse = true;
    }
    expect(!mouse, "CANCEL bound a mouse button");

    editor::Editor in_well(editor_fixture());
    editor::ScreenState capturing;
    capturing.selected = "order.attack";
    editor::editor_command(in_well, capturing, unused, editor::EditorCommand::add_key);
    const kit::DisplayList again = editor::editor_layout(in_well, capturing, frame, fonts());
    if (const std::optional<kit::Point> at = centre_named(again, editor::name_capture_well)) {
        editor::editor_pointer(in_well, capturing, again, editor::PointerKind::down, 1, *at, 0);
        expect(capturing.capture.chord.has_value(), "a click in the well stored no chord");
        if (capturing.capture.chord) {
            expect(
                capturing.capture.chord->kind == keys::TriggerKind::mouse,
                "the well did not bind the button"
            );
            expect(capturing.capture.chord->button == 1, "the well stored another button");
        }
    }

    editor::Editor turning(editor_fixture());
    editor::ScreenState spinning;
    spinning.selected = "order.attack";
    editor::editor_command(turning, spinning, unused, editor::EditorCommand::add_key);
    const kit::DisplayList wheel_list = editor::editor_layout(turning, spinning, frame, fonts());
    if (const std::optional<kit::Point> at = centre_named(wheel_list, editor::name_capture_well)) {
        editor::editor_wheel(turning, spinning, wheel_list, *at, 1.F);
        expect(spinning.capture.chord.has_value(), "a wheel turn in the well stored no chord");
        if (spinning.capture.chord)
            expect(
                spinning.capture.chord->kind == keys::TriggerKind::wheel,
                "the well did not bind the wheel"
            );
    }
}

} // namespace

int main() {
    held_modifier_then_key();
    button_and_wheel();
    means_and_gestures();
    lone_shift();
    escape_timer();
    refused_and_settings();
    tab_is_not_a_clash();
    replace_swap_cancel();
    capture_prompt_shape();
    clash_row_words();
    outcomes_wait_for_confirm();
    no_swap_for_add();
    card_choices();
    find_by_key();
    limits();
    well_not_the_buttons();
    return oa::test::failed_checks() == 0 ? 0 : 1;
}
