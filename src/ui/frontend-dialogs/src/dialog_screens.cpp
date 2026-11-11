// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Overlay registration, input routing and the public dialog API.
#include "dialog_internal.hpp"

#include "oa/ui/gui_input.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oa::ui::frontend_dialogs {

namespace {

constexpr int16_t kOverlayZ = 30000; // above every screen widget

// Active button under the pointer; labels are not activation targets. The
// pointer path tests root-relative record rectangles.
int32_t hit_button(const Dialog& dialog, float x, float y) {
    const auto& gadgets = dialog.resources.layout.gadgets;
    const auto& root = gadgets.front().common;
    const ui::gui_input::MenuObject menu{gadgets, kNoGadget};
    const auto found = ui::gui_input::hit_test(
        menu, static_cast<int32_t>(x) - root.x, static_cast<int32_t>(y) - root.y
    );
    if (!found || *found >= gadgets.size() ||
        gadgets[*found].common.type != ui::gui_layout::GadgetType::button)
        return kNoGadget;
    return static_cast<int32_t>(*found);
}

// A multi-stage button advances its stage before the handler runs, then the
// dialog's handler receives the record name. The dialog may be released.
void activate(app::ScreenContext* ctx, Dialog& dialog, int32_t index) {
    auto& gadgets = dialog.resources.layout.gadgets;
    if (index <= 0 || static_cast<std::size_t>(index) >= gadgets.size())
        return;
    auto& gadget = gadgets[static_cast<std::size_t>(index)];
    const auto* button = std::get_if<ui::gui_layout::ButtonFields>(&gadget.fields);
    if (button == nullptr || gadget.common.active == 0)
        return;
    const auto at = static_cast<std::size_t>(index);
    if (at < dialog.stages.size())
        dialog.stages[at] = ui::gui_input::released_button_stage(gadget, dialog.stages[at]);
    const std::string name = gadget.common.name;
    switch (dialog.kind) {
    case DialogKind::message_box:
        message_box_click(ctx, dialog, name);
        break;
    case DialogKind::cd_check:
        cd_check_dialog_click(ctx, dialog, name);
        break;
    case DialogKind::help:
        help_click(ctx, dialog, name);
        break;
    case DialogKind::continue_watching:
        continue_watching_click(dialog, name);
        break;
    case DialogKind::notice:
        notice_click(dialog, name);
        break;
    case DialogKind::none:
        break;
    }
}

// Rebuild the input records from the current layout, retaining the dialog's focus.
void focus_key(app::ScreenContext* ctx, Dialog& dialog, const app::ScreenInput& input) {
    auto panel = std::make_unique<ui::gui_input::GadgetPanel>();
    ui::gui_input::init_gadget_panel(*panel);
    auto* owner = ui::gui_input::load_panel(
        *panel, dialog.resources.layout, dialog.name, panel_flag::no_draw
    );
    if (owner == nullptr)
        return;
    if (dialog.focus_initialized)
        owner->focus = dialog.focus;
    uint16_t modifiers = input.modifiers;
    panel->host.context = &modifiers;
    panel->host.is_key_down = [](void* context, int32_t key) {
        return key == ui::gui_input::key_code::shift &&
               (*static_cast<const uint16_t*>(context) & 0x0003) != 0;
    };
    ui::gui_input::dispatch_key(*panel, static_cast<int32_t>(input.key));
    dialog.focus = owner->focus;
    dialog.focus_initialized = true;
    if (panel->activated != kNoGadget)
        activate(ctx, dialog, panel->activated);
}

std::string default_button(const Dialog& dialog, uint32_t key) {
    const auto* panel =
        std::get_if<ui::gui_layout::PanelFields>(&dialog.resources.layout.gadgets.front().fields);
    if (panel == nullptr)
        return {};
    if (key == static_cast<uint32_t>(ui::gui_input::key_code::enter))
        return panel->carriage_return_default;
    if (key == static_cast<uint32_t>(ui::gui_input::key_code::escape))
        return panel->escape_default;
    return {};
}

/// Finds the button of a yes-or-no prompt whose quick key a key is, in either case.
///
/// Only the YESORNO prompts take quick keys: Y and N in English, or the
/// letters their translated captions give them. A key with Ctrl, Alt or the
/// system key down types no character and finds none; a grayed-out or
/// inactive button takes no key.
///
/// @param dialog the top dialog
/// @param input the key press
/// @return the button's record, or kNoGadget
int32_t quick_key_button(const Dialog& dialog, const app::ScreenInput& input) {
    if (dialog.kind != DialogKind::continue_watching ||
        (input.modifiers &
         (app::kInputModifierCtrl | app::kInputModifierAlt | app::kInputModifierSystem)) != 0 ||
        input.key == 0 || input.key >= 0x80)
        return kNoGadget;
    const auto typed = std::tolower(static_cast<int>(input.key));
    const auto& gadgets = dialog.resources.layout.gadgets;
    for (std::size_t index = 1; index < gadgets.size(); ++index) {
        const auto* button = std::get_if<ui::gui_layout::ButtonFields>(&gadgets[index].fields);
        if (button == nullptr || gadgets[index].common.active == 0 || button->grayed_out ||
            button->quick_key == 0)
            continue;
        if (std::tolower(static_cast<unsigned char>(button->quick_key)) == typed)
            return static_cast<int32_t>(index);
    }
    return kNoGadget;
}

/// Tells whether typed text is the character of the quick key that answered a prompt.
///
/// @param text the typed text, UTF-8
/// @param answered the quick key, lowercase, or zero for none
/// @return true for that one character, in either case
bool typed_answered_key(const char* text, int answered) {
    return answered != 0 && text != nullptr && text[0] != '\0' && text[1] == '\0' &&
           std::tolower(static_cast<unsigned char>(text[0])) == answered;
}

} // namespace

int dialog_event(app::ScreenContext* ctx, void*) {
    if (ctx == nullptr || ctx->input == nullptr)
        return 0;
    const auto& input = *ctx->input;
    // The character a quick key types after its press answered a prompt goes
    // nowhere, though the prompt is closed by then.
    auto& answered = dialog_stack().answered_key;
    if (input.kind == app::ScreenInputKind::key_down)
        answered = 0;
    else if (
        input.kind == app::ScreenInputKind::text &&
        typed_answered_key(input.text, std::exchange(answered, 0))
    )
        return 1;
    auto* dialog = dialog_top();
    if (dialog == nullptr)
        return 0;
    switch (input.kind) {
    case app::ScreenInputKind::pointer_move:
        dialog->hovered = hit_button(*dialog, input.x, input.y);
        return 1;
    case app::ScreenInputKind::pointer_down:
        if (input.button == ui::gui_input::kLeftButton)
            dialog->pressed = hit_button(*dialog, input.x, input.y);
        return 1;
    case app::ScreenInputKind::pointer_up: {
        if (input.button != ui::gui_input::kLeftButton)
            return 1;
        const auto released = hit_button(*dialog, input.x, input.y);
        const auto pressed = dialog->pressed;
        dialog->pressed = kNoGadget;
        if (released != kNoGadget && released == pressed)
            activate(ctx, *dialog, released);
        return 1;
    }
    case app::ScreenInputKind::key_down: {
        if (input.key == ui::gui_input::key_code::tab ||
            input.key == ui::gui_input::key_code::space) {
            focus_key(ctx, *dialog, input);
            return 1;
        }
        const auto name = default_button(*dialog, input.key);
        if (!name.empty()) {
            const auto index = dialog_find(*dialog, name);
            if (index != kNoGadget)
                activate(ctx, *dialog, index);
            return 1;
        }
        if (const auto index = quick_key_button(*dialog, input); index != kNoGadget) {
            dialog_stack().answered_key = std::tolower(static_cast<int>(input.key));
            activate(ctx, *dialog, index);
        }
        return 1;
    }
    case app::ScreenInputKind::key_up:
    case app::ScreenInputKind::text:
    case app::ScreenInputKind::wheel:
        return 1;
    }
    return 1;
}

void dialog_draw_hook(app::ScreenContext* ctx, void*) {
    dialog_draw(ctx);
}

namespace {

void compose_stack(renderer::Surface& frame, const uint8_t* layer_key) {
    auto& stack = dialog_stack();
    std::size_t index = 0;
    while (index < stack.count) {
        std::string error;
        if (dialog_compose(
                stack.dialogs[index],
                index > 0 ? &stack.dialogs[index - 1] : nullptr,
                frame,
                &error,
                layer_key
            )) {
            ++index;
            continue;
        }
        // A dialog that cannot be drawn would still take every input.
        const auto message = "cannot draw a frontend dialog: " + error;
        if (stack.host.report != nullptr)
            stack.host.report(stack.host.context, message.c_str());
        dialog_close(stack.dialogs[index]);
    }
}

bool uses_colour(const PaletteBytes& palette, const uint8_t* rgb) {
    for (std::size_t at = 0; at < palette.size(); at += palette_entry_bytes)
        if (palette[at] == rgb[0] && palette[at + 1] == rgb[1] && palette[at + 2] == rgb[2])
            return true;
    return false;
}

} // namespace

void dialog_draw(app::ScreenContext* ctx) {
    if (ctx == nullptr || ctx->surface == nullptr)
        return;
    const auto& host = dialog_stack().host;
    if (host.draws_layer != nullptr && host.draws_layer(host.context))
        return;
    compose_stack(*ctx->surface, nullptr);
}

void dialog_draw_layer(uint32_t width, uint32_t height, std::vector<uint8_t>& rgba) {
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    rgba.assign(pixels * 4U, 0);
    const auto* bottom = dialog_top() != nullptr ? &dialog_stack().dialogs[0] : nullptr;
    if (bottom == nullptr || pixels == 0)
        return;
    // Uncovered pixels keep a colour no panel palette holds.
    const auto palette = dialog_palette(*bottom);
    uint8_t key[3] = {1, 2, 3};
    while (uses_colour(palette, key) || uses_colour(bottom->resources.gui_palette, key))
        ++key[2];
    renderer::Surface layer{width, height, std::vector<uint8_t>(pixels * 3U)};
    for (std::size_t at = 0; at < pixels; ++at)
        std::copy_n(key, 3, &layer.rgb[at * 3U]);
    compose_stack(layer, key);
    for (std::size_t at = 0; at < pixels; ++at) {
        const auto* source = &layer.rgb[at * 3U];
        if (source[0] == key[0] && source[1] == key[1] && source[2] == key[2])
            continue;
        std::copy_n(source, 3, &rgba[at * 4U]);
        rgba[at * 4U + 3U] = 0xff;
    }
}

void dialog_shade_below(
    renderer::Surface& frame,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const PaletteBytes& palette
) {
    auto& stack = dialog_stack();
    if (stack.count == 0 || (stack.dialogs[0].flags & panel_flag::shade_below) == 0)
        return;
    dialog_shade(frame, x, y, width, height, palette, nullptr);
}

void dialogs_bind_host(const DialogHost& host) noexcept {
    dialog_stack().host = host;
}

void close_dialog() {
    dialog_pop();
}

void reset_dialogs() {
    while (dialog_top() != nullptr)
        dialog_pop();
}

DialogKind dialog_kind() noexcept {
    const auto* dialog = dialog_top();
    return dialog == nullptr ? DialogKind::none : dialog->kind;
}

std::size_t dialog_count() noexcept {
    return dialog_stack().count;
}

const renderer::ScreenResources* dialog_resources() noexcept {
    const auto* dialog = dialog_top();
    return dialog == nullptr ? nullptr : &dialog->resources;
}

std::string_view dialog_layout_name() noexcept {
    const auto* dialog = dialog_top();
    return dialog == nullptr ? std::string_view() : std::string_view(dialog->name);
}

int32_t help_page() noexcept {
    const auto* dialog = dialog_top();
    return dialog == nullptr || dialog->kind != DialogKind::help ? 0 : dialog->help_page;
}

uint8_t dialog_button_stage(std::size_t index) noexcept {
    const auto* dialog = dialog_top();
    if (dialog == nullptr || index >= dialog->stages.size())
        return 0;
    return dialog->stages[index];
}

int32_t dialog_keyboard_focus() noexcept {
    const Dialog* dialog = dialog_top();
    if (dialog == nullptr || !dialog->focus_initialized || dialog->focus <= 0)
        return -1;
    if (static_cast<std::size_t>(dialog->focus) >= dialog->resources.layout.gadgets.size())
        return -1;
    return dialog->focus;
}

bool dialog_click(app::ScreenContext* ctx, const char* name) {
    auto* dialog = dialog_top();
    if (dialog == nullptr || name == nullptr)
        return false;
    const auto index = dialog_find(*dialog, name);
    if (index == kNoGadget)
        return false;
    activate(ctx, *dialog, index);
    return true;
}

} // namespace oa::ui::frontend_dialogs

namespace oa::app {

void register_frontend_dialog_screens(ScreenRegistry* registry) {
    OverlayDesc overlay{};
    overlay.name = "frontend dialogs";
    overlay.screen = kScreenAny;
    overlay.z = oa::ui::frontend_dialogs::kOverlayZ;
    overlay.event = oa::ui::frontend_dialogs::dialog_event;
    overlay.draw = oa::ui::frontend_dialogs::dialog_draw_hook;
    (void)overlay_register(registry, &overlay);
}

} // namespace oa::app
