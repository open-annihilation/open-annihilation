// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The keyboard and mouse pictures of a clean OA default layout. Every key of
// every shape is a code the bindings know. Labels follow what the keyboard
// prints. No key is dashed as fixed. A free key's click fills K33's prompt
// and does not bind. The mouse's scheme redraws roles and leaves the layout.

#include "oa/data/input_actions/actions.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/key_editor/model.hpp"
#include "oa/ui/key_editor/screen.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/keys.hpp"
#include "oa/ui/kit/looks.hpp"
#include "screen_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace editor = oa::ui::key_editor;
namespace keys = oa::ui::key_bindings;
namespace acts = oa::data::input_actions;
namespace oakeys = oa::formats::oakeys;
using key_editor_fixture::fonts;
using key_editor_fixture::frame_for;
using key_editor_fixture::has_room;
using key_editor_fixture::press_area;

/// A clean OA default editor: no own bindings, the US keyboard, right-click.
///
/// @return the inputs
editor::EditorInputs stock_inputs() {
    editor::EditorInputs inputs;
    inputs.base = keys::Base::oa_default;
    inputs.own.base = keys::Base::oa_default;
    inputs.keyboard = keys::KeyboardMap::us();
    inputs.scheme = keys::PointerScheme::right_click;
    return inputs;
}

/// The editor, its screen and the list last laid out, at Large.
struct Board {
    editor::Editor model;
    editor::ScreenState state{};
    kit::Frame frame{frame_for(1280, 720, 100)};
    kit::DisplayList list{};

    /// Lays a clean editor out.
    ///
    /// @param inputs the editor's inputs
    explicit Board(editor::EditorInputs inputs) : model(std::move(inputs)) { show(); }

    /// Lays the screen out again.
    void show() { list = editor::editor_layout(model, state, frame, fonts()); }

    /// Lays the screen out again when the result asks for it.
    ///
    /// @param result what the input returned
    void take(editor::EditorResult result) {
        if (result.redraw)
            show();
    }

    /// A control of a name, or null.
    ///
    /// @param name the name
    /// @return the control
    const kit::Control* find(std::string_view name) const {
        return kit::control_of(list, kit::control_named(list, name));
    }

    /// Presses a control at the centre of what a press can reach.
    ///
    /// @param name the control
    void click(std::string_view name) {
        const kit::Control* control = find(name);
        if (control == nullptr || !has_room(press_area(*control)))
            std::fprintf(stderr, "missing %.*s\n", static_cast<int>(name.size()), name.data());
        OA_CHECK(control != nullptr && has_room(press_area(*control)));
        if (control == nullptr || !has_room(press_area(*control)))
            return;
        const kit::Rect area = press_area(*control);
        const kit::Point at{area.x + area.width / 2, area.y + area.height / 2};
        take(editor::editor_pointer(model, state, list, editor::PointerKind::down, 1, at, 0));
        take(editor::editor_pointer(model, state, list, editor::PointerKind::up, 1, at, 0));
    }
};

/// The picture's name of one key.
///
/// @param code the W3C code
/// @return key-editor.keyboard.<code word>
std::string key_name(std::string_view code) {
    return std::string(editor::name_keyboard) + "." + kit::code_word(code);
}

/// One key of a picture, or null. The look must outlive the pointer.
///
/// @param look the picture
/// @param code the W3C code
/// @return the key
const kit::KeyboardKey* key_of(const kit::KeyboardLook& look, std::string_view code) {
    for (const kit::KeyboardKey& key : look.keys) {
        if (key.code == code)
            return &key;
    }
    return nullptr;
}

/// The picture for one shape and one layer.
///
/// @param model the editor
/// @param shape which keyboard
/// @param layer the modifier bit, or 0
/// @return the picture
kit::KeyboardLook picture_of(const editor::Editor& model, kit::KeyboardShape shape, uint8_t layer) {
    editor::KeyboardViewInputs inputs;
    inputs.shape = shape;
    inputs.layer = layer;
    return editor::keyboard_look(model, inputs, editor::key_labels({}));
}

/// True when the table has that role on that button while playing.
///
/// @param rows the table
/// @param button the button's word
/// @param role the action's name
/// @return true when a row says so
bool playing_role(
    const std::vector<editor::RoleLine>& rows, std::string_view button, std::string_view role
) {
    for (const editor::RoleLine& row : rows) {
        if (row.place_id == "playing" && row.button == button && row.role == role)
            return true;
    }
    return false;
}

/// Every key of every shape is a code the bindings know.
void every_code_is_known() {
    const kit::KeyboardShape shapes[] = {
        kit::KeyboardShape::ansi, kit::KeyboardShape::iso, kit::KeyboardShape::jis
    };
    int count = 0;
    for (const kit::KeyboardShape shape : shapes) {
        for (const kit::KeyPlace& place : kit::keyboard_places(shape)) {
            if (!keys::scancode_of(place.code))
                std::fprintf(stderr, "no scancode for %s\n", std::string(place.code).c_str());
            OA_CHECK(keys::scancode_of(place.code).has_value());
            ++count;
        }
    }
    OA_CHECK(count > 0);
}

/// Labels, layers, Escape, Settings and an AZERTY map. No key is dashed.
void labels_and_layers() {
    const editor::Editor plain(stock_inputs());
    const kit::KeyboardShape shapes[] = {
        kit::KeyboardShape::ansi, kit::KeyboardShape::iso, kit::KeyboardShape::jis
    };
    const uint8_t layers[] = {0, keys::shift, keys::ctrl, keys::alt, keys::meta};
    for (const kit::KeyboardShape shape : shapes) {
        for (const uint8_t layer : layers) {
            const kit::KeyboardLook look = picture_of(plain, shape, layer);
            for (const kit::KeyboardKey& key : look.keys) {
                if (key.cap.mark == kit::KeyMark::fixed)
                    std::fprintf(stderr, "dashed %s\n", key.code.c_str());
                OA_CHECK(key.cap.mark != kit::KeyMark::fixed);
                if (key.cap.tint != kit::KeyTint::none && key.cap.letter.empty())
                    std::fprintf(stderr, "tint without a letter on %s\n", key.code.c_str());
                OA_CHECK(key.cap.tint == kit::KeyTint::none || !key.cap.letter.empty());
            }
        }
    }

    const kit::KeyboardLook none = picture_of(plain, kit::KeyboardShape::ansi, 0);
    const kit::KeyboardKey* attack = key_of(none, "KeyA");
    const kit::KeyboardKey* page = key_of(none, "Digit1");
    const kit::KeyboardKey* escape = key_of(none, "Escape");
    const kit::KeyboardKey* tab = key_of(none, "Tab");
    OA_CHECK(attack != nullptr && attack->cap.label == "A" && attack->cap.sub == "Attack");
    OA_CHECK(page != nullptr && page->cap.sub == "Page 1");
    OA_CHECK(escape != nullptr && escape->cap.sub == "Back");
    OA_CHECK(escape->cap.mark != kit::KeyMark::fixed);
    OA_CHECK(tab != nullptr && tab->cap.sub == "Next");

    const std::vector<editor::CardLine> escape_card = editor::key_card(plain, "Escape", 0);
    bool saw_back = false;
    bool saw_chat = false;
    bool saw_screens = false;
    for (const editor::CardLine& line : escape_card) {
        if (line.id == "game.back" && line.action == "Back" && !line.fixed)
            saw_back = true;
        if (line.id == "chat.cancel" && line.fixed && line.context == "In the chat line" &&
            line.action == "Close the line" && line.note == "always")
            saw_chat = true;
        if (line.id == "ui.screen-cancel" && line.fixed &&
            line.context == "On Open Annihilation's screens" && line.action == "Back" &&
            line.note == "always")
            saw_screens = true;
    }
    if (!saw_back || !saw_chat || !saw_screens)
        std::fprintf(stderr, "Escape card is missing a line\n");
    OA_CHECK(saw_back && saw_chat && saw_screens);

    const std::vector<editor::CardLine> tab_card = editor::key_card(plain, "Tab", 0);
    bool next_primary = false;
    bool next_control = false;
    for (const editor::CardLine& line : tab_card) {
        if (line.id == "select.next-primary" && line.context == "While playing")
            next_primary = true;
        if (line.id == "ui.screen-focus-next" && !line.context.empty())
            next_control = true;
    }
    OA_CHECK(next_primary && next_control);

    const kit::KeyboardLook ctrl = picture_of(plain, kit::KeyboardShape::ansi, keys::ctrl);
    const kit::KeyboardKey* builders = key_of(ctrl, "KeyB");
    OA_CHECK(builders != nullptr && builders->cap.sub == "Sel bld");
    const kit::KeyboardLook alt = picture_of(plain, kit::KeyboardShape::ansi, keys::alt);
    const kit::KeyboardKey* group = key_of(alt, "Digit1");
    OA_CHECK(group != nullptr && group->cap.sub == "Group 1");

    editor::EditorInputs hacked_inputs = stock_inputs();
    hacked_inputs.hacks_on = {"ui.selection-shortcuts"};
    const editor::Editor hacked(std::move(hacked_inputs));
    const kit::KeyboardLook hacked_ctrl = picture_of(hacked, kit::KeyboardShape::ansi, keys::ctrl);
    const kit::KeyboardKey* idle = key_of(hacked_ctrl, "KeyB");
    OA_CHECK(idle != nullptr && idle->cap.sub == "Idle bld");
    const std::vector<editor::CardLine> idle_card = editor::key_card(hacked, "KeyB", keys::ctrl);
    bool idle_note = false;
    for (const editor::CardLine& line : idle_card) {
        if (line.id == "select.idle-builders" &&
            line.note.find("in place of") != std::string::npos &&
            line.note.find("Select builders") != std::string::npos)
            idle_note = true;
    }
    OA_CHECK(idle_note);

    editor::EditorInputs windows = stock_inputs();
    windows.platform.primary_is_meta = false;
    const editor::Editor windows_editor(windows);
    const kit::KeyboardLook windows_ctrl =
        picture_of(windows_editor, kit::KeyboardShape::ansi, keys::ctrl);
    const kit::KeyboardKey* windows_comma = key_of(windows_ctrl, "Comma");
    OA_CHECK(
        windows_comma != nullptr && windows_comma->cap.sub == "Settings" &&
        windows_comma->cap.mark != kit::KeyMark::fixed
    );
    const std::vector<editor::CardLine> windows_card =
        editor::key_card(windows_editor, "Comma", keys::ctrl);
    bool settings_plain = false;
    for (const editor::CardLine& line : windows_card) {
        if (line.id == "ui.settings" && line.note.find("always") == std::string::npos)
            settings_plain = true;
    }
    OA_CHECK(settings_plain);

    editor::EditorInputs mac = stock_inputs();
    mac.platform.primary_is_meta = true;
    const editor::Editor mac_editor(mac);
    const kit::KeyboardLook mac_cmd = picture_of(mac_editor, kit::KeyboardShape::ansi, keys::meta);
    const kit::KeyboardKey* mac_comma = key_of(mac_cmd, "Comma");
    OA_CHECK(
        mac_comma != nullptr && mac_comma->cap.sub == "Settings" &&
        mac_comma->cap.mark != kit::KeyMark::fixed
    );

    editor::EditorInputs azerty_inputs = stock_inputs();
    const std::optional<uint16_t> key_q = keys::scancode_of("KeyQ");
    const std::optional<uint16_t> key_a = keys::scancode_of("KeyA");
    OA_CHECK(key_q.has_value() && key_a.has_value());
    if (key_q && key_a) {
        azerty_inputs.keyboard.set(*key_q, U'a', U'a');
        azerty_inputs.keyboard.set(*key_a, U'q', U'q');
    }
    const editor::Editor azerty(std::move(azerty_inputs));
    const kit::KeyboardLook azerty_none = picture_of(azerty, kit::KeyboardShape::ansi, 0);
    const kit::KeyboardKey* azerty_q = key_of(azerty_none, "KeyQ");
    const kit::KeyboardKey* azerty_a = key_of(azerty_none, "KeyA");
    OA_CHECK(azerty_q != nullptr && azerty_q->cap.label == "A" && azerty_q->cap.sub == "Attack");
    OA_CHECK(azerty_a != nullptr && azerty_a->cap.label == "Q" && azerty_a->cap.sub != "Attack");
}

/// A click on a key only inspects it: the inspector names what it does and
/// nothing changes. Change {action}'s key… opens the capture for that chord;
/// Use {key} for {action}… fills the capture for the action chosen in the
/// list, which waits for its button.
void clicks() {
    {
        Board board(stock_inputs());
        board.state.view = editor::View::keyboard;
        board.state.layer = keys::ctrl;
        board.show();
        board.click(key_name("Comma"));
        OA_CHECK(board.state.card_code == "Comma");
        OA_CHECK(board.state.prompt == editor::Prompt::none);
        board.show();
        const kit::Control* card =
            kit::control_of(board.list, kit::control_named(board.list, editor::name_key_card));
        OA_CHECK(card != nullptr && card->text.find("Settings") != std::string::npos);
        const kit::Control* change =
            kit::control_of(board.list, kit::control_named(board.list, editor::name_card_change));
        OA_CHECK(change != nullptr && change->text == "Change Settings's key…");
    }
    {
        Board board(stock_inputs());
        board.state.view = editor::View::keyboard;
        board.show();
        board.click(key_name("KeyA"));
        OA_CHECK(board.state.selected.empty());
        OA_CHECK(board.state.prompt == editor::Prompt::none);
        board.show();
        board.click(editor::name_card_change);
        if (board.state.selected != "order.attack")
            std::fprintf(stderr, "A's inspector chose %s\n", board.state.selected.c_str());
        OA_CHECK(board.state.selected == "order.attack");
        OA_CHECK(board.state.prompt == editor::Prompt::capture);
        OA_CHECK(board.state.capture.replacing.has_value());
        OA_CHECK(board.model.changes() == 0);
    }
    {
        Board board(stock_inputs());
        board.state.view = editor::View::keyboard;
        board.state.layer = keys::shift;
        board.state.selected = "order.repair";
        board.show();
        const std::string before = oakeys::write_bindings_text(board.model.result().manifest);
        board.click(key_name("KeyA"));
        OA_CHECK(board.state.prompt == editor::Prompt::none);
        board.show();
        board.click(editor::name_card_use);
        const keys::Chord* chord =
            board.state.capture.chord ? &*board.state.capture.chord : nullptr;
        if (chord == nullptr || chord->kind != keys::TriggerKind::character ||
            chord->character != U'A' || chord->modifiers != keys::shift)
            std::fprintf(stderr, "Shift+A was not the letter chord\n");
        OA_CHECK(board.state.prompt == editor::Prompt::capture);
        OA_CHECK(chord != nullptr);
        OA_CHECK(chord->kind == keys::TriggerKind::character);
        OA_CHECK(chord->character == U'A');
        OA_CHECK(chord->modifiers == keys::shift);
        OA_CHECK(chord->gesture == keys::Gesture::press);
        OA_CHECK(board.state.capture.means == acts::By::letter);
        OA_CHECK(board.state.capture.state == editor::CaptureState::complete);
        OA_CHECK(board.model.changes() == 0);
        OA_CHECK(oakeys::write_bindings_text(board.model.result().manifest) == before);
    }
    {
        Board board(stock_inputs());
        board.state.view = editor::View::keyboard;
        board.state.selected = "order.repair";
        board.show();
        const std::string before = oakeys::write_bindings_text(board.model.result().manifest);
        board.click(key_name("F9"));
        OA_CHECK(board.state.prompt == editor::Prompt::none);
        board.show();
        board.click(editor::name_card_use);
        const keys::Chord* chord =
            board.state.capture.chord ? &*board.state.capture.chord : nullptr;
        OA_CHECK(board.state.prompt == editor::Prompt::capture);
        OA_CHECK(chord != nullptr);
        OA_CHECK(chord->kind == keys::TriggerKind::named);
        OA_CHECK(chord->named == static_cast<uint8_t>(keys::NamedKey::f9));
        OA_CHECK(chord->modifiers == 0);
        OA_CHECK(board.state.capture.means == acts::By::place);
        OA_CHECK(board.state.capture.state == editor::CaptureState::complete);
        OA_CHECK(board.model.changes() == 0);
        OA_CHECK(oakeys::write_bindings_text(board.model.result().manifest) == before);
    }
}

/// Both schemes move Select and Command. The layout's bindings stay.
void mouse_schemes() {
    const editor::Editor model(stock_inputs());
    const std::vector<editor::RoleLine> right =
        editor::mouse_table(model, keys::PointerScheme::right_click);
    const std::vector<editor::RoleLine> left =
        editor::mouse_table(model, keys::PointerScheme::left_click);
    OA_CHECK(playing_role(right, "Left", "Select"));
    OA_CHECK(playing_role(right, "Right", "Command"));
    OA_CHECK(playing_role(left, "Left", "Command"));
    const kit::MouseLook right_look = editor::mouse_look(model, keys::PointerScheme::right_click);
    const kit::MouseLook left_look = editor::mouse_look(model, keys::PointerScheme::left_click);
    OA_CHECK(
        right_look.buttons[static_cast<std::size_t>(kit::MouseButton::right)].label == "Command"
    );
    OA_CHECK(left_look.buttons[static_cast<std::size_t>(kit::MouseButton::left)].label == "Select");
    OA_CHECK(model.changes() == 0);
    OA_CHECK(model.unsaved() == 0);

    editor::Editor draft(stock_inputs());
    const std::string before = oakeys::write_bindings_text(draft.result().manifest);
    draft.set_scheme(keys::PointerScheme::left_click);
    OA_CHECK(draft.scheme() == keys::PointerScheme::left_click);
    OA_CHECK(draft.changes() == 0);
    OA_CHECK(draft.unsaved() == 1);
    OA_CHECK(oakeys::write_bindings_text(draft.result().manifest) == before);
    OA_CHECK(playing_role(editor::mouse_table(draft, draft.scheme()), "Left", "Command"));
}

/// Each mouse button reads as its roles to automation, at Large and at
/// Compact, as a key reads as its legend: Left as Select, Right as Command.
void mouse_buttons_read_their_roles() {
    for (const kit::Frame& frame : {frame_for(1280, 720, 100), frame_for(640, 480, 100)}) {
        Board board(stock_inputs());
        board.frame = frame;
        board.state.view = editor::View::mouse;
        board.show();
        const kit::Control* left = board.find("key-editor.mouse.mouse-left");
        const kit::Control* right = board.find("key-editor.mouse.mouse-right");
        const kit::Control* wheel = board.find("key-editor.mouse.wheel");
        OA_CHECK(left != nullptr && left->text.find("Select") != std::string::npos);
        OA_CHECK(right != nullptr && right->text.find("Command") != std::string::npos);
        OA_CHECK(wheel != nullptr && !wheel->text.empty());
        if (left != nullptr && left->text.find("Select") == std::string::npos)
            std::fprintf(stderr, "the left button reads \"%s\"\n", left->text.c_str());
    }
}

/// FIND ON KEYBOARD outlines Attack and focuses KeyA.
void find_outlines_attack() {
    Board board(stock_inputs());
    board.state.selected = "order.attack";
    board.state.details_more = true;
    board.show();
    const std::vector<std::string> codes = editor::keys_of(board.model, "order.attack");
    bool key_a = false;
    for (const std::string& code : codes) {
        if (code == "KeyA")
            key_a = true;
    }
    OA_CHECK(key_a);
    board.click(editor::name_detail_find);
    OA_CHECK(board.state.outline == "order.attack");
    OA_CHECK(board.state.view == editor::View::keyboard);
    OA_CHECK(board.state.focus_code == "KeyA");
    bool outlined = false;
    for (const kit::Item& item : board.list.items) {
        const auto* look = std::get_if<kit::KeyboardLook>(&item.look);
        if (look == nullptr)
            continue;
        const kit::KeyboardKey* key = key_of(*look, "KeyA");
        if (key != nullptr && key->cap.focused)
            outlined = true;
    }
    OA_CHECK(outlined);
}

} // namespace

int main() {
    every_code_is_known();
    labels_and_layers();
    clicks();
    mouse_schemes();
    mouse_buttons_read_their_roles();
    find_outlines_attack();
    return oa::test::failed_checks() == 0 ? 0 : 1;
}
